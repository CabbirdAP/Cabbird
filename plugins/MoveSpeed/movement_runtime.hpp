#pragma once

struct Vector3 final { float x{},y{},z{}; };
static_assert(sizeof(Vector3)==12 && alignof(Vector3)==alignof(float));
// The verified native Vector3 return uses RCX as its output and returns that
// pointer in RAX; self and hidden MethodInfo follow it. Keep both results.
using DesiredFn=Vector3*(CABBIRD_CALL*)(Vector3*,std::uintptr_t,std::uintptr_t);
using RootMotionFn=Vector3*(CABBIRD_CALL*)(Vector3*,std::uintptr_t,float,std::uintptr_t);
using FloatFn=float(CABBIRD_CALL*)(std::uintptr_t,std::uintptr_t);
using SetFloatFn=void(CABBIRD_CALL*)(std::uintptr_t,float,std::uintptr_t);
using DecorateFn=void(CABBIRD_CALL*)(std::uintptr_t,std::uintptr_t,std::uintptr_t);
using MoveFn=void(CABBIRD_CALL*)(std::uintptr_t,bool,std::uintptr_t);
using UpdateFn=void(CABBIRD_CALL*)(std::uintptr_t,std::uintptr_t);

class CallbackLease final {
public:
    CallbackLease(Context& c,Native id) noexcept:hooks_(c.hooks) {
        held_=Succeeded(hooks_->begin_callback(hooks_->user,c.methods[id].handle,&lease_));
    }
    ~CallbackLease(){if(held_)static_cast<void>(hooks_->end_callback(hooks_->user,lease_));}
    explicit operator bool() const noexcept{return held_;}
    CallbackLease(const CallbackLease&)=delete;
    CallbackLease& operator=(const CallbackLease&)=delete;
private:
    const CabbirdHookServiceV1* hooks_{};
    CabbirdGenerationHandleV1 lease_{};
    bool held_{};
};
bool OnGame(const Context& c) noexcept {
    return c.game_thread.load(std::memory_order_acquire)==GetCurrentThreadId();
}
template<class T>
bool ReadValue(Context& c,std::uintptr_t object,std::uint32_t offset,T& value) noexcept {
    return object && Succeeded(c.core->read_memory(c.core->user,object+offset,
        {reinterpret_cast<std::uint8_t*>(&value),sizeof(value)}));
}
Owner EligibleOwner(Context& c,std::uintptr_t self,std::uint32_t owner_offset) noexcept {
    Owner owner;
    if(!c.requested_enabled.load(std::memory_order_relaxed))return owner;
    owner.factor=c.factor.load(std::memory_order_relaxed);
    if(owner.factor==1)return {};
    std::uintptr_t handle{};
    if(!ReadValue(c,self,owner_offset,handle) || !ReadValue(c,handle,c.handle_entity,owner.entity) ||
        !ReadValue(c,owner.entity,c.entity_data,owner.data) || !owner.data)return {};
    if(c.player_only.load(std::memory_order_relaxed)) {
        CabbirdUnityPlayerSnapshotV1 snapshot{};snapshot.struct_size=sizeof(snapshot);
        if(!Succeeded(c.player->snapshot(c.player->user,&snapshot)) || !(snapshot.flags&CABBIRD_UNITY_PLAYER_V1_VALID) ||
            snapshot.entity_id!=owner.data)return {};
    }
    owner.state=self;
    return owner;
}
void MultiplyHorizontal(Vector3& v,float factor) noexcept {v.x*=factor;v.z*=factor;}
template<std::size_t N>
bool IsState(Context& c,std::uintptr_t self,const std::array<std::uint32_t,N>& allowed) noexcept {
    std::uintptr_t data{};std::uint32_t type{};
    if(!ReadValue(c,self,c.state_data,data) || !ReadValue(c,data,c.state_type,type))return false;
    for(const auto expected:allowed)if(type==expected)return true;
    return false;
}
struct LocomotionScope final {
    Context& c;Owner previous;
    LocomotionScope(Context& context,Owner owner):c(context),previous(c.locomotion){c.locomotion=owner;}
    ~LocomotionScope(){c.locomotion=previous;}
};

template<Native Id>
Vector3* CABBIRD_CALL DesiredDetour(Vector3* result,std::uintptr_t self,std::uintptr_t method) noexcept {
    auto* c=g_active.load(std::memory_order_acquire);
    if(!c){*result={};return result;}
    const CallbackLease lease(*c,Id);
    if(!lease){*result={};return result;} // No lease means no trampoline lifetime.
    try {
        auto* returned=reinterpret_cast<DesiredFn>(c->methods[Id].original)(result,self,method);
        if(!OnGame(*c))return returned;
        Owner owner;
        if constexpr(Id==FlightCurve || Id==HookFlightCurve) {
            // These leaves participate only in the fully installed consumer
            // scope, sharing its identity/factor and later speed cap.
            owner=c->locomotion;
            if(owner.state!=self || !owner.flight)return returned;
        } else owner=EligibleOwner(*c,self,c->state_entity);
        if(owner.state) {
            if constexpr(Id==MountJumpCurve) {
                // The shared base also serves water jumps; only the two
                // independently verified ground state configurations qualify.
                if(!IsState(*c,self,c->mount_jump_states))return returned;
            }
            MultiplyHorizontal(*returned,owner.factor);
            if constexpr(Id==FlightCurve || Id==FlightSprint || Id==HookFlightCurve || Id==HookFlightSprint)
                returned->y*=owner.factor;
        }
        return returned;
    } catch (...) {*result={};return result;}
}
template<Native Id>
void CABBIRD_CALL MoveDetour(std::uintptr_t self,bool moving,std::uintptr_t method) noexcept {
    auto* c=g_active.load(std::memory_order_acquire);if(!c)return;
    const CallbackLease lease(*c,Id);if(!lease)return;
    try {
        const auto original=reinterpret_cast<MoveFn>(c->methods[Id].original);
        if(!OnGame(*c)){original(self,moving,method);return;}
        Owner owner=EligibleOwner(*c,self,c->state_entity);
        if constexpr(Id==BaseMove)if(owner.state && !IsState(*c,self,c->move_states))owner={};
        if constexpr(Id==FlightMove || Id==FlightIdleMove || Id==HookFlightMove || Id==HookFlightIdleMove)
            owner.flight=true;
        // Keep the scope around native input consumers, including JumpLoop's
        // independent air-control cap. Inherited Fall velocity is already
        // physical speed; never multiply that complete result again.
        const LocomotionScope scope(*c,owner);
        reinterpret_cast<MoveFn>(c->methods[Id].original)(self,moving,method);
    } catch (...) {}
}
void CABBIRD_CALL StandJumpDetour(std::uintptr_t self,std::uintptr_t method) noexcept {
    auto* c=g_active.load(std::memory_order_acquire);if(!c)return;
    const CallbackLease lease(*c,StandJumpUpdate);if(!lease)return;
    try {
        const auto original=reinterpret_cast<UpdateFn>(c->methods[StandJumpUpdate].original);
        if(!OnGame(*c)){original(self,method);return;}
        const LocomotionScope scope(*c,EligibleOwner(*c,self,c->state_entity));
        original(self,method);
    } catch (...) {}
}
template<Native Id>
float CABBIRD_CALL LimitDetour(std::uintptr_t self,std::uintptr_t method) noexcept {
    auto* c=g_active.load(std::memory_order_acquire);if(!c)return 0;
    const CallbackLease lease(*c,Id);if(!lease)return 0;
    float value{};
    try {
        value=reinterpret_cast<FloatFn>(c->methods[Id].original)(self,method);
        if(OnGame(*c) && c->locomotion.state==self)value*=c->locomotion.factor;
        return value;
    } catch (...) {return value;}
}
Vector3* CABBIRD_CALL RootMotionDetour(Vector3* result,std::uintptr_t self,float delta,std::uintptr_t method) noexcept {
    auto* c=g_active.load(std::memory_order_acquire);if(!c){*result={};return result;}
    const CallbackLease lease(*c,RootMotion);if(!lease){*result={};return result;}
    try {
        auto* returned=reinterpret_cast<RootMotionFn>(c->methods[RootMotion].original)(result,self,delta,method);
        // AliveEntity hides Entity.data; compare the actual entity from the
        // outer state, never that shadow field. Flight alone scales vertical
        // locomotion; ordinary jump curves and gravity retain their native Y.
        if(OnGame(*c) && c->locomotion.state && c->locomotion.entity==self) {
            MultiplyHorizontal(*returned,c->locomotion.factor);
            if(c->locomotion.flight)returned->y*=c->locomotion.factor;
        }
        return returned;
    } catch (...) {*result={};return result;}
}
void CABBIRD_CALL WorldDetour(std::uintptr_t self,std::uintptr_t state,std::uintptr_t method) noexcept {
    auto* c=g_active.load(std::memory_order_acquire);if(!c)return;
    const CallbackLease lease(*c,World);if(!lease)return;
    try {
        reinterpret_cast<DecorateFn>(c->methods[World].original)(self,state,method);
        if(!OnGame(*c))return;
        const Owner owner=EligibleOwner(*c,self,c->steering_entity);
        if(!owner.state)return;
        // Original Decorate overwrites TargetSpeed from its unmodified cache.
        // The following pipeline consumes it with native acceleration/deceleration.
        const float base=reinterpret_cast<FloatFn>(c->methods[TargetSpeed].target)(state,0);
        reinterpret_cast<SetFloatFn>(c->methods[SetTargetSpeed].target)(state,base*owner.factor,0);
    } catch (...) {}
}
void* Detour(Native id) noexcept {
    switch(id) {
    case BaseMove:return reinterpret_cast<void*>(&MoveDetour<BaseMove>);
    case Controllable:return reinterpret_cast<void*>(&DesiredDetour<Controllable>);
    case MountRun:return reinterpret_cast<void*>(&DesiredDetour<MountRun>);
    case MountSprint:return reinterpret_cast<void*>(&DesiredDetour<MountSprint>);
    case CurveJump:return reinterpret_cast<void*>(&DesiredDetour<CurveJump>);
    case CurveDouble:return reinterpret_cast<void*>(&DesiredDetour<CurveDouble>);
    case FastJump:return reinterpret_cast<void*>(&DesiredDetour<FastJump>);
    case MountJumpCurve:return reinterpret_cast<void*>(&DesiredDetour<MountJumpCurve>);
    case JumpLoopMove:return reinterpret_cast<void*>(&MoveDetour<JumpLoopMove>);
    case StandJumpUpdate:return reinterpret_cast<void*>(&StandJumpDetour);
    case FlightMove:return reinterpret_cast<void*>(&MoveDetour<FlightMove>);
    case FlightIdleMove:return reinterpret_cast<void*>(&MoveDetour<FlightIdleMove>);
    case FlightCurve:return reinterpret_cast<void*>(&DesiredDetour<FlightCurve>);
    case FlightLimit:return reinterpret_cast<void*>(&LimitDetour<FlightLimit>);
    case FlightSprint:return reinterpret_cast<void*>(&DesiredDetour<FlightSprint>);
    case HookFlightMove:return reinterpret_cast<void*>(&MoveDetour<HookFlightMove>);
    case HookFlightIdleMove:return reinterpret_cast<void*>(&MoveDetour<HookFlightIdleMove>);
    case HookFlightCurve:return reinterpret_cast<void*>(&DesiredDetour<HookFlightCurve>);
    case HookFlightLimit:return reinterpret_cast<void*>(&LimitDetour<HookFlightLimit>);
    case HookFlightSprint:return reinterpret_cast<void*>(&DesiredDetour<HookFlightSprint>);
    case World:return reinterpret_cast<void*>(&WorldDetour);
    case SpeedLimit:return reinterpret_cast<void*>(&LimitDetour<SpeedLimit>);
    case JumpLoopLimit:return reinterpret_cast<void*>(&LimitDetour<JumpLoopLimit>);
    case RootMotion:return reinterpret_cast<void*>(&RootMotionDetour);
    default:return nullptr;
    }
}
bool Temporary(CabbirdStatusV1 status) noexcept {
    return status.code==CABBIRD_STATUS_V1_UNAVAILABLE || status.code==CABBIRD_STATUS_V1_NOT_FOUND;
}
BindingState ResolveField(Context& c,std::string_view type,std::string_view name,std::string_view field_type,
                          std::uint32_t& offset,std::uint32_t size) {
    CabbirdUnityFieldRequestV1 request{};request.struct_size=sizeof(request);request.storage_size=size;
    request.type_name=StringView(type);request.field_name=StringView(name);request.field_type=StringView(field_type);
    CabbirdUnityFieldResultV1 result{};result.struct_size=sizeof(result);
    const auto status=c.metadata->resolve_instance_field(c.metadata->user,&request,&result);
    if(Temporary(status))return BindingState::waiting;
    if(!Succeeded(status) || result.storage_size!=size || result.offset>64U*1024U ||
        result.offset<sizeof(std::uintptr_t)*2 || result.offset%size!=0)return BindingState::fields;
    offset=result.offset;return BindingState::ready;
}
bool ResolveMethod(Context& c,Native id) {
    auto& slot=c.methods[id];
    if(slot.handle.id || slot.state.load()!=BindingState::waiting)return slot.state.load()==BindingState::ready;
    const auto& source=kMethods[id];
    const std::string type=std::string(kWorld)+source.type;
    const auto parameter=StringView(source.parameter?source.parameter:"");
    CabbirdUnityMethodRequestV1 request{};request.struct_size=sizeof(request);request.method_kind=CABBIRD_UNITY_METHOD_V1_INSTANCE;
    request.module_name=StringView("GameAssembly.dll");request.type_name=StringView(type);request.method_name=StringView(source.name);
    request.return_type=StringView(source.result);request.parameter_types=source.parameter?&parameter:nullptr;request.parameter_count=source.parameter?1:0;
    CabbirdUnityMethodResultV1 result{};result.struct_size=sizeof(result);
    const auto status=c.metadata->resolve_method(c.metadata->user,&request,&result);
    if(Temporary(status))return false;
    if(!Succeeded(status) || !result.address) {slot.state.store(BindingState::metadata);return false;}
    slot.target=result.address;slot.state.store(BindingState::ready);return true;
}
bool Install(Context& c,Native id) {
    auto& slot=c.methods[id];if(slot.handle.id)return true;
    if(slot.state.load()!=BindingState::ready)return false;
    CabbirdHookRequestV1 request{};request.struct_size=sizeof(request);request.kind=CABBIRD_HOOK_V1_FUNCTION;
    request.target=slot.target;request.detour=Detour(id);request.label=StringView(kMethods[id].symbol);
    const auto status=c.hooks->create(c.hooks->user,&request,&slot.original,&slot.handle);
    if(!Succeeded(status)) {slot.state.store(BindingState::hook);return false;}
    return true;
}
bool NeedsAutomaticBinding(const Context& c) noexcept {
    for(const auto& slot:c.methods)if(slot.state.load()==BindingState::waiting)return true;
    return false;
}
CabbirdStatusV1 ReadStateValues(Context& c,const CabbirdIl2CppApiV1* api) {
    if(!api || api->struct_size<offsetof(CabbirdIl2CppApiV1,il2cpp_field_get_flags)+sizeof(api->il2cpp_field_get_flags) ||
        !api->il2cpp_domain_get || !api->il2cpp_domain_get_assemblies || !api->il2cpp_assembly_get_image ||
        !api->il2cpp_image_get_name || !api->il2cpp_class_from_name || !api->il2cpp_class_get_field_from_name ||
        !api->il2cpp_field_get_type || !api->il2cpp_type_get_name || !api->il2cpp_free || !api->il2cpp_field_get_flags || !api->il2cpp_field_static_get_value)
        return {CABBIRD_STATUS_V1_UNAVAILABLE,0,{}};
    const auto domain=api->il2cpp_domain_get();if(!domain)return {CABBIRD_STATUS_V1_UNAVAILABLE,0,{}};
    std::size_t count{};const auto assemblies=api->il2cpp_domain_get_assemblies(domain,&count);
    CabbirdIl2CppClassV1* type{};
    for(std::size_t i{};i<count;++i) {
        const auto image=api->il2cpp_assembly_get_image(assemblies[i]);
        const auto name=api->il2cpp_image_get_name(image);
        if(!name || (std::string_view(name)!="Assembly-CSharp" && std::string_view(name)!="Assembly-CSharp.dll"))continue;
        type=api->il2cpp_class_from_name(image,"Lens.Gameplay.Modules.BigWorld","EFsmStateType");break;
    }
    if(!type)return {CABBIRD_STATUS_V1_UNAVAILABLE,0,{}};
    const auto matches=[&](CabbirdFieldInfoV1* field,std::string_view expected) {
        if(!field)return false;
        const auto name=api->il2cpp_type_get_name(api->il2cpp_field_get_type(field));
        const bool result=name && std::string_view(name)==expected;
        if(name)api->il2cpp_free(const_cast<char*>(name));
        return result;
    };
    const auto underlying=api->il2cpp_class_get_field_from_name(type,"value__");
    if(!matches(underlying,"System.Int32") || (api->il2cpp_field_get_flags(underlying)&0x10))
        return {CABBIRD_STATUS_V1_FAILED,0,{}};
    constexpr std::array<const char*,7> names{"Run","FastRun","Fall","FastRunFall","FallHigh","CurveJump","CurveDoubleJump"};
    std::array<std::uint32_t,7> values{};
    for(std::size_t i{};i<names.size();++i) {
        const auto field=api->il2cpp_class_get_field_from_name(type,names[i]);
        if(!matches(field,"Lens.Gameplay.Modules.BigWorld.EFsmStateType") || (api->il2cpp_field_get_flags(field)&0x50)!=0x50)
            return {CABBIRD_STATUS_V1_FAILED,0,{}};
        api->il2cpp_field_static_get_value(field,&values[i]);
        for(std::size_t j{};j<i;++j)if(values[i]==values[j])return {CABBIRD_STATUS_V1_FAILED,0,{}};
    }
    std::copy_n(values.begin(),c.move_states.size(),c.move_states.begin());
    c.mount_jump_states={values[5],values[6]};c.states_ready=true;
    return Ok();
}
CabbirdStatusV1 CABBIRD_CALL ResolveStateValues(void* user,const CabbirdIl2CppApiV1* api) noexcept {
    try {return ReadStateValues(*static_cast<Context*>(user),api);}
    catch (...) {return {CABBIRD_STATUS_V1_FAILED,0,{}};}
}
void BindMovement(Context& c) {
    if(!c.ownership_ready) {
        struct Field {const char *type,*name,*storage;std::uint32_t *offset,size;};
        const std::array<Field,6> fields{{
            {"Common.BaseState","m_entity","EntityHandle",&c.state_entity,8},
            {"MoveSystem.SteeringBehavior","m_entity","EntityHandle",&c.steering_entity,8},
            {"EntityHandle","<entity>k__BackingField","Entity",&c.handle_entity,8},
            {"Entity","data","BaseData",&c.entity_data,8},
            {"Common.BaseState","<stateData>k__BackingField","FsmStateData",&c.state_data,8},
            {"FsmStateData","stateType","EFsmStateType",&c.state_type,4}
        }};
        for(const auto& field:fields) {
            const auto state=ResolveField(c,std::string(kWorld)+field.type,field.name,std::string(kWorld)+field.storage,*field.offset,field.size);
            if(state!=BindingState::ready) {
                for(auto& slot:c.methods)slot.state.store(state);
                if(state==BindingState::fields)c.bind_issue=c.localizer.Format("log.field","Move Speed: incompatible field {0}.{1}.",std::array<std::string_view,2>{field.type,field.name});
                return;
            }
        }
        c.ownership_ready=true;
    }
    if(!c.states_ready) {
        const auto status=c.il2cpp->with_runtime(c.il2cpp->user,ResolveStateValues,&c);
        if(!Succeeded(status)) {
            if(!Temporary(status)) {
                for(auto& slot:c.methods)slot.state.store(BindingState::metadata);
                c.bind_issue=c.localizer.Text("log.states","Move Speed: locomotion state metadata is incompatible; movement remains unchanged.");
            }
            return;
        }
    }
    for(std::size_t i{};i<NativeCount;++i)static_cast<void>(ResolveMethod(c,static_cast<Native>(i)));
    // Different signatures must never patch one folded/wrapper entry twice.
    for(std::size_t i{};i<NativeCount;++i)for(std::size_t j=i+1;j<NativeCount;++j)
        if(c.methods[i].target && c.methods[i].target==c.methods[j].target) {
            c.methods[i].state.store(BindingState::alias);c.methods[j].state.store(BindingState::alias);
        }
    // Install every input leaf before any scope that enables it.
    const bool leaves=Install(c,SpeedLimit) && Install(c,RootMotion);
    const bool loop=Install(c,JumpLoopLimit);
    if(leaves) {static_cast<void>(Install(c,BaseMove));static_cast<void>(Install(c,StandJumpUpdate));}
    if(leaves && loop)static_cast<void>(Install(c,JumpLoopMove));
    for(const Native id:{Controllable,MountRun,MountSprint,CurveJump,CurveDouble,FastJump,MountJumpCurve})static_cast<void>(Install(c,id));
    // Flight curves are fresh locomotion vectors. Fly/Idle scopes cover the
    // original builder and its later 3D cap; never multiply cached physical
    // velocity after Flying, which would feed the factor into native braking.
    const bool flight=Install(c,FlightCurve) && Install(c,FlightLimit);
    const bool hook_flight=Install(c,HookFlightCurve) && Install(c,HookFlightLimit);
    static_cast<void>(Install(c,FlightSprint));static_cast<void>(Install(c,HookFlightSprint));
    if(leaves) {
        static_cast<void>(Install(c,FlightIdleMove));static_cast<void>(Install(c,HookFlightIdleMove));
        if(flight)static_cast<void>(Install(c,FlightMove));
        if(hook_flight)static_cast<void>(Install(c,HookFlightMove));
    }
    if(c.methods[TargetSpeed].state.load()==BindingState::ready && c.methods[SetTargetSpeed].state.load()==BindingState::ready)
        static_cast<void>(Install(c,World));
    // Log the first actual incompatibility, once per changed cause. Ordinary
    // absent-world / runtime-not-ready waiting is deliberately silent.
    c.bind_issue.clear();
    for(std::size_t i{};i<NativeCount;++i) {
        const auto state=c.methods[i].state.load();
        if(state==BindingState::waiting || state==BindingState::ready)continue;
        if(state==BindingState::metadata)c.bind_issue=c.localizer.Format("log.metadata","Move Speed: method metadata differs for {0}; this path remains unchanged.",std::array<std::string_view,1>{kMethods[i].symbol});
        else if(state==BindingState::alias)c.bind_issue=c.localizer.Format("log.alias","Move Speed: {0} shares an incompatible native entry; this path remains unchanged.",std::array<std::string_view,1>{kMethods[i].symbol});
        else c.bind_issue=c.localizer.Format("log.hook","Move Speed: the host could not create the hook for {0}; this path remains unchanged.",std::array<std::string_view,1>{kMethods[i].symbol});
        break;
    }
}
void ReportBinding(Context& c) {
    if(c.bind_issue.empty() || c.bind_issue==c.last_bind_issue)return;
    c.last_bind_issue=c.bind_issue;
    c.core->log(c.core->user,CABBIRD_CORE_LOG_LEVEL_V1_WARNING,StringView(c.bind_issue));
}
