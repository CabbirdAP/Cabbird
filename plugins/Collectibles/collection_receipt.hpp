#pragma once
#include "cabbird/sdk/profile.hpp"
#include "cabbird/sdk/services/interop.h"
#include <atomic>
#include <array>
#include <string>
#include <cstring>
#include <sstream>
#include <windows.h>
namespace collectibles {
// Observes only POD fields of the game's own response while its native callback
// owns the objects. No IL2CPP API or borrowed response survives that callback.
class RewardReceipt final {
public:
    bool Bind(const cabbird::sdk::Host& host, const cabbird::sdk::Profile& profile, const CabbirdCoreServiceV1* core) {
        core_ = core;
        const auto hooks = host.Query<CabbirdHookServiceV1>(CABBIRD_HOOK_SERVICE_V1_ID);
        if (!hooks || hooks->struct_size < sizeof(*hooks.get()) || !hooks->create || !hooks->release || !hooks->begin_callback || !hooks->end_callback) return false;
        hooks_ = hooks.get();
        const char* names[]{"receiptResponseList","receiptListItems","receiptListSize","receiptEntryObject","receiptEntryRewards","receiptObjectId","receiptObjectComplete","receiptRewardsList","receiptItemAmount","receiptNullableValue","receiptNullableBoolValue","receiptArrayData","receiptNullableHas"};
        for (std::size_t i{};i<std::size(names);++i) offsets_[i]=profile.Value(names[i],4096);
        if(offsets_[9]+sizeof(std::uint32_t)>8 || offsets_[10]>=2 || offsets_[12]>=2)return false;
        // These native bodies independently witness the nested protobuf and
        // collection layouts, which the public class-name resolver cannot find.
        for(const char* name:{"receiptResponse","receiptObjectIdGetter","receiptObjectCompleteGetter","receiptEntryObjectGetter","receiptEntryRewardsGetter","receiptRewardsGetter","receiptAmountGetter","receiptCollectionWitness"}) {
            std::uintptr_t address{};
            if(!ValidateProof(profile,name,address))return false;
            if(std::string_view(name)=="receiptResponse")entry_=address;
        }
        return entry_!=0;
    }
    template<class Runtime> bool Validate(const Runtime& runtime, const CabbirdUnityMetadataServiceV1* metadata) const {
        struct FieldShape {const char* klass;const char* name;std::uint32_t storage;std::size_t slot;};
        const FieldShape fields[]{{"SCWorldMapObjInteract","<objs>k__BackingField",sizeof(void*),0},{"WorldObj","__pbn__objId",8,5},{"WorldObj","__pbn__complete",2,6},{"Rewards","<rewards>k__BackingField",sizeof(void*),7},{"CommonItem","__pbn__itemnum",8,8}};
        for(const auto& field:fields){
            auto* klass=runtime.FindClassAnyImage("Protocal.Cs",field.klass);auto* info=runtime.FindField(klass,field.name);
            if(!info)return false;
            const auto type=runtime.TypeName(runtime.Functions().il2cpp_field_get_type(info));const auto name=std::string("Protocal.Cs.")+field.klass;
            CabbirdUnityFieldRequestV1 request{sizeof(request),field.storage,cabbird::sdk::StringView(name),cabbird::sdk::StringView(field.name),cabbird::sdk::StringView(type)};
            CabbirdUnityFieldResultV1 result{sizeof(result)};
            if(!cabbird::sdk::Succeeded(metadata->resolve_instance_field(metadata->user,&request,&result))||result.offset!=offsets_[field.slot])return false;
        }
        return runtime.ArrayElementBase()==offsets_[11];
    }
    bool Install(std::uintptr_t verified) {
        gameThread_.store(GetCurrentThreadId(),std::memory_order_release);
        if(handle_.id)return true;
        if(verified!=entry_)return false;
        active_.store(this,std::memory_order_release);
        const CabbirdHookRequestV1 request{sizeof(request),CABBIRD_HOOK_V1_FUNCTION,entry_,reinterpret_cast<void*>(&Observe),cabbird::sdk::StringView("collectibles.reward-response")};
        if(!cabbird::sdk::Succeeded(hooks_->create(hooks_->user,&request,&original_,&handle_))){active_.store(nullptr,std::memory_order_release);return false;}
        return true;
    }
    bool CheckProof(const cabbird::sdk::Profile& profile,std::string_view name) const {
        std::uintptr_t address{};
        return ValidateProof(profile,name,address);
    }
    void Arm(std::int32_t mapId) const {
        const auto token=(++serial_<<32)|static_cast<std::uint32_t>(mapId);
        callbackCount_.store(0,std::memory_order_relaxed);foreignThreadCallbacks_.store(0,std::memory_order_relaxed);
        lastResponseCode_.store(0,std::memory_order_relaxed);
        inspection_ = {};
        seen_.store(0,std::memory_order_relaxed);armed_.store(token,std::memory_order_release);
    }
    void Cancel() const {armed_.store(0,std::memory_order_release);}
    bool Confirmed() const {
        const auto token=armed_.load(std::memory_order_acquire);
        return token && seen_.load(std::memory_order_acquire)==token && static_cast<std::int32_t>(payload_>>32)==0 && static_cast<std::uint32_t>(payload_)!=0 && responseComplete_;
    }
    std::string Details() const {
        const auto token=armed_.load(std::memory_order_acquire);
        std::ostringstream out;const bool seen=token&&seen_.load(std::memory_order_acquire)==token;
        out<<" actionRequest="<<(token>>32)<<" receiptSeen="<<seen;
        const auto callbacks=token?callbackCount_.load(std::memory_order_relaxed):0;
        out<<" nativeCallbacks="<<callbacks<<" foreignThreadCallbacks="<<(token?foreignThreadCallbacks_.load(std::memory_order_relaxed):0);
        if(callbacks)out<<" nativeErrorCode="<<lastResponseCode_.load(std::memory_order_relaxed);
        if(token&&inspection_.stage)out<<" receiptParse="<<inspection_.stage<<" responseObjects="<<inspection_.objects
            <<" responseMapIds=["<<inspection_.ids[0]<<','<<inspection_.ids[1]<<"] responseIdPresentMask="<<unsigned(inspection_.present);
        if(seen)out<<" responseCode="<<static_cast<std::int32_t>(payload_>>32)<<" rewardItems="<<static_cast<std::uint32_t>(payload_)<<" responseComplete="<<responseComplete_;
        return out.str();
    }
    bool Stop() {
        Cancel();
        if(handle_.id){const auto status=hooks_->release(hooks_->user,handle_);if(!cabbird::sdk::Succeeded(status)&&status.code!=CABBIRD_STATUS_V1_NOT_FOUND)return false;handle_={};}
        active_.store(nullptr,std::memory_order_release);return true;
    }
private:
    bool ValidateProof(const cabbird::sdk::Profile& profile,std::string_view name,std::uintptr_t& address) const {
        const auto main=cabbird::sdk::ResolveProfileSymbolRva(core_,profile,name);
        if(!main||!main.candidate||main.candidate->entry.empty()||main.candidate->entry.size()>64)return false;
        if(name=="receiptCollectionWitness") {
            const auto offset=main.candidate->values.find("ownerOffset");
            if(offset==main.candidate->values.end()||main.address!=entry_+offset->second)return false;
        }
        const auto span=main.candidate->values.find("proofSpan");
        const auto parts=main.candidate->values.find("proofParts");
        if(span==main.candidate->values.end()||parts==main.candidate->values.end()||
            span->second>4096||span->second<main.candidate->entry.size()||parts->second==0||parts->second>64)return false;
        std::size_t covered=main.candidate->entry.size();
        for(std::uint32_t index=1;index<parts->second;++index) {
            const auto part=cabbird::sdk::ResolveProfileSymbolRva(core_,profile,std::string(name)+".part"+std::to_string(index));
            if(!part||!part.candidate||part.candidate->entry.empty()||part.candidate->entry.size()>64)return false;
            const auto offset=part.candidate->values.find("proofOffset");
            if(offset==part.candidate->values.end()||offset->second>=covered||offset->second>span->second||
                part.candidate->entry.size()>span->second-offset->second||
                part.address!=main.address+offset->second)return false;
            const auto end=offset->second+part.candidate->entry.size();
            if(end<=covered)return false;
            covered=end;
        }
        if(covered!=span->second)return false;
        address=main.address;
        return true;
    }
    using Original=void(CABBIRD_CALL*)(std::uintptr_t,std::int32_t,std::uintptr_t,std::uintptr_t);
    struct Inspection {
        const char* stage{};
        std::int32_t objects{-1};
        std::array<std::uint32_t,2> ids{};
        std::uint8_t present{};
    };
    template<class T>bool Read(std::uintptr_t object,std::uint32_t offset,T& out)const noexcept {
        return object&&cabbird::sdk::Succeeded(core_->read_memory(core_->user,object+offset,{reinterpret_cast<std::uint8_t*>(&out),sizeof(out)}));
    }
    bool List(std::uintptr_t list,std::uintptr_t& items,std::int32_t& count)const noexcept {
        return Read(list,offsets_[1],items)&&Read(list,offsets_[2],count)&&count>=0&&count<=64&&(count==0||items);
    }
    std::uint32_t Inspect(std::uintptr_t response,std::uint32_t target,bool& complete,bool& matched,Inspection& inspection)const noexcept {
        std::uintptr_t list{},items{};std::int32_t count{};
        inspection.stage="response-list";
        if(!Read(response,offsets_[0],list))return 0;
        inspection.stage="object-list";
        const auto valid=List(list,items,count);inspection.objects=count;
        if(!valid)return 0;
        inspection.stage="object-id";
        for(std::int32_t i{};i<count;++i){
            std::uintptr_t entry{},object{};std::array<std::uint8_t,8> id{};
            if(!Read(items,offsets_[11]+i*sizeof(void*),entry)||!Read(entry,offsets_[3],object)||!Read(object,offsets_[5],id))continue;
            std::uint32_t actual{};std::memcpy(&actual,id.data()+offsets_[9],sizeof(actual));
            if(i<2){inspection.ids[i]=actual;if(id[offsets_[12]])inspection.present|=static_cast<std::uint8_t>(1u<<i);}
            if(!id[offsets_[12]]||actual!=target)continue;matched=true;
            inspection.stage="target-rewards";
            std::array<std::uint8_t,2> done{};if(Read(object,offsets_[6],done))complete=done[offsets_[12]]&&done[offsets_[10]];
            std::uintptr_t reward{},rewardList{},rewardItems{};std::int32_t rewards{};
            if(!Read(entry,offsets_[4],reward)||!Read(reward,offsets_[7],rewardList)||!List(rewardList,rewardItems,rewards))return 0;
            std::uint32_t positive{};
            for(std::int32_t j{};j<rewards;++j){std::uintptr_t item{};std::array<std::uint8_t,8> quantity{};std::uint32_t amount{};
                if(Read(rewardItems,offsets_[11]+j*sizeof(void*),item)&&Read(item,offsets_[8],quantity)&&quantity[offsets_[12]]){std::memcpy(&amount,quantity.data()+offsets_[9],sizeof(amount));if(amount)++positive;}}
            inspection.stage="target-read";return positive;
        }
        return 0;
    }
    static void CABBIRD_CALL Observe(std::uintptr_t self,std::int32_t code,std::uintptr_t response,std::uintptr_t method) noexcept {
        auto* state=active_.load(std::memory_order_acquire);if(!state)return;
        CabbirdGenerationHandleV1 lease{};const bool held=cabbird::sdk::Succeeded(state->hooks_->begin_callback(state->hooks_->user,state->handle_,&lease));
        __try { ObserveBody(state,held,self,code,response,method); }
        __finally { if(held)static_cast<void>(state->hooks_->end_callback(state->hooks_->user,lease)); }
    }
    static void ObserveBody(RewardReceipt* state,bool held,std::uintptr_t self,std::int32_t code,std::uintptr_t response,std::uintptr_t method) {
        bool called{};
        try {
            const auto token=state->armed_.load(std::memory_order_acquire);
            std::uint32_t items{};bool complete{},matched{};
            Inspection inspection;
            const bool gameThread=GetCurrentThreadId()==state->gameThread_.load(std::memory_order_acquire);
            if(held&&token){
                state->callbackCount_.fetch_add(1,std::memory_order_relaxed);
                state->lastResponseCode_.store(code,std::memory_order_relaxed);
                if(!gameThread)state->foreignThreadCallbacks_.fetch_add(1,std::memory_order_relaxed);
            }
            const bool observe=held&&token&&gameThread;
            if(observe&&code==0)items=state->Inspect(response,static_cast<std::uint32_t>(token),complete,matched,inspection);
            called=true;reinterpret_cast<Original>(state->original_)(self,code,response,method);
            if(observe&&state->armed_.load(std::memory_order_acquire)==token)state->inspection_=inspection;
            if(observe&&matched&&state->armed_.load(std::memory_order_acquire)==token&&!(state->seen_.load(std::memory_order_acquire)==token&&static_cast<std::uint32_t>(state->payload_)&&state->responseComplete_)){state->payload_=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(code))<<32)|items;state->responseComplete_=complete;state->seen_.store(token,std::memory_order_release);}
        } catch(...) {if(!called&&state->original_)try{reinterpret_cast<Original>(state->original_)(self,code,response,method);}catch(...){} }
    }
    const CabbirdCoreServiceV1* core_{};const CabbirdHookServiceV1* hooks_{};
    std::array<std::uint32_t,13> offsets_{};std::uintptr_t entry_{},original_{};CabbirdGenerationHandleV1 handle_{};
    std::atomic<DWORD> gameThread_{};
    mutable std::atomic<std::uint64_t> armed_{},seen_{};
    mutable std::atomic<std::uint32_t> callbackCount_{},foreignThreadCallbacks_{};
    mutable std::atomic<std::int32_t> lastResponseCode_{};
    mutable Inspection inspection_;
    mutable std::uint64_t serial_{};std::uint64_t payload_{};bool responseComplete_{};
    inline static std::atomic<RewardReceipt*> active_{};
};
}
