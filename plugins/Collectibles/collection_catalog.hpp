#pragma once
// Included after the private runtime invocation helpers in collection_runtime.hpp.
#include <unordered_map>
#include <unordered_set>

namespace collectibles {
struct CatalogEntry {
    std::int32_t collectionId{}, typeId{};
    std::string category, name;
};
struct CatalogMethods {
    const Method *typeId{}, *typeName{}, *typeShow{}, *collectionId{}, *collectionType{}, *collectionName{}, *spawners{}, *langValue{}, *tableReady{};
};
class CollectionCatalog final {
public:
    void Bind(CatalogMethods methods) { methods_ = methods; }
    bool Cancel(const CabbirdIl2CppServiceV1* service) {
        CabbirdIl2CppGCHandleV1 handles[2]{}; std::size_t count{};
        if (enumerator_) handles[count++] = enumerator_;
        if (spawners_) handles[count++] = spawners_;
        if (count && (!service || !service->release_handles || !Succeeded(service->release_handles(service->user, handles, count)))) return false;
        enumerator_ = nullptr; spawners_ = nullptr; stage_ = Stage::Done;
        return true;
    }
    void Begin() {
        entries_.clear(); types_.clear(); ambiguous_.clear(); readyGroups_.clear(); absentGroups_.clear();
        stage_ = Stage::Types; typeRows_ = collectionRows_ = missingNames_ = hiddenRows_ = cachedTables_ = 0;
        typesReady_ = rowsReady_ = false; detail_.clear();
    }
    bool Pending() const { return stage_ != Stage::Done; }
    bool Ready() const { return rowsReady_; }
    const CatalogEntry* Find(std::int32_t spawner) const {
        const auto found = entries_.find(spawner); return found == entries_.end() ? nullptr : &found->second;
    }
    std::string Summary() const {
        return "catalogTypeReady=" + std::to_string(typesReady_) + " catalogReady=" + std::to_string(rowsReady_) +
            " catalogTypes=" + std::to_string(typeRows_) + " catalogRows=" + std::to_string(collectionRows_) +
            " catalogSpawners=" + std::to_string(entries_.size()) + " nameFallbacks=" + std::to_string(missingNames_) +
            " catalogHiddenRows=" + std::to_string(hiddenRows_) + " catalogAmbiguousSpawners=" + std::to_string(ambiguous_.size()) +
            " catalogCachedTables=" + std::to_string(cachedTables_) +
            (detail_.empty() ? "" : " catalogDetails=" + detail_);
    }
    void Step(const Runtime& runtime) {
        std::uint32_t budget = 32;
        while (budget && stage_ != Stage::Done) {
            if (spawners_) {
                auto* list = runtime.Functions().il2cpp_gchandle_get_target(spawners_);
                while (budget && spawnerIndex_ < spawnerCount_) {
                    auto index = spawnerIndex_++; --budget; std::int32_t spawner{}; void* args[]{&index};
                    if (!Value(runtime, Member(runtime, list, "get_Item", {"System.Int32"}, "System.Int32"), list, args, spawner)) { StopTable(runtime, "collection spawner list invocation failed"); break; }
                    if (spawner > 0) AddEntry(spawner, current_);
                }
                if (spawners_ && spawnerIndex_ == spawnerCount_) Free(runtime, spawners_);
                if (!budget || spawners_) continue;
            }
            const bool typeStage = stage_ == Stage::Types;
            const auto* tableName = typeStage ? "TDWorldCollectingTypeTable" : "TDWorldCollectingTable";
            const auto* rowType = typeStage ? "Azur.Gameplay.Table.TDWorldCollectingType" : "Azur.Gameplay.Table.TDWorldCollecting";
            if (!enumerator_) {
                auto* table = PreparedTable(runtime, tableName);
                if (!table) { StopTable(runtime, std::string(tableName) + " not prepared in memory: " + prepareFailure_); continue; }
                if (!StartEnumerator(runtime, table, rowType)) { StopTable(runtime, std::string(tableName) + " enumerator unavailable"); continue; }
            }
            auto* iterator = runtime.Functions().il2cpp_gchandle_get_target(enumerator_);
            bool more{};
            if (!StructValue(runtime, move_, iterator, more)) { StopTable(runtime, std::string(tableName) + " MoveNext failed"); continue; }
            if (!more) {
                if (typeStage) typesReady_ = true; else rowsReady_ = true;
                Free(runtime, enumerator_); stage_ = typeStage ? Stage::Collections : Stage::Done; continue;
            }
            --budget;
            Object* pair{};
            if (!StructInvoke(runtime, currentMethod_, runtime.Functions().il2cpp_gchandle_get_target(enumerator_), pair) || !pair) { StopTable(runtime, "catalog get_Current failed"); continue; }
            Root pairRoot(runtime, pair); Object* row{};
            const auto* value = Member(runtime, pairRoot.get(), "get_Value", {}, rowType);
            if (!StructInvoke(runtime, value, pairRoot.get(), row) || !row) { StopTable(runtime, "catalog pair.get_Value failed"); continue; }
            Root rowRoot(runtime, row);
            if (typeStage) {
                std::int32_t id{}, show{};
                if (!StructValue(runtime, methods_.typeId, rowRoot.get(), id) || !StructValue(runtime, methods_.typeShow, rowRoot.get(), show)) { StopTable(runtime, "TDWorldCollectingType getters failed"); continue; }
                ++typeRows_;
                if (show == 1) types_[id] = RowName(runtime, rowRoot.get(), methods_.typeName);
            } else {
                CatalogEntry entry;
                if (!StructValue(runtime, methods_.collectionId, rowRoot.get(), entry.collectionId) ||
                    !StructValue(runtime, methods_.collectionType, rowRoot.get(), entry.typeId)) { StopTable(runtime, "TDWorldCollecting getters failed"); continue; }
                ++collectionRows_;
                const auto category = types_.find(entry.typeId);
                // The game's collection tabs expose only show == 1 types.
                if (typesReady_ && category == types_.end()) { ++hiddenRows_; continue; }
                if (category != types_.end()) entry.category = category->second;
                entry.name = RowName(runtime, rowRoot.get(), methods_.collectionName);
                Object* list{};
                if (!StructInvoke(runtime, methods_.spawners, rowRoot.get(), list)) { StopTable(runtime, "TDWorldCollecting.get_spawnerId failed"); continue; }
                if (!list) continue;
                Root listRoot(runtime, list); std::int32_t count{};
                if (!Value(runtime, Member(runtime, listRoot.get(), "get_Count", {}, "System.Int32"), listRoot.get(), nullptr, count) || count < 0) { StopTable(runtime, "TDWorldCollecting spawner count failed"); continue; }
                spawners_ = runtime.Functions().il2cpp_gchandle_new(listRoot.get(), false);
                if (!spawners_) { StopTable(runtime, "TDWorldCollecting spawner handle unavailable"); continue; }
                current_ = std::move(entry); spawnerIndex_ = 0; spawnerCount_ = count;
            }
        }
    }
private:
    enum class Stage { Types, Collections, Done } stage_{Stage::Done};
    void AddEntry(std::int32_t spawner, const CatalogEntry& entry) {
        const auto [found, inserted] = entries_.try_emplace(spawner, entry);
        if (inserted) return;
        auto& previous = found->second;
        if (previous.collectionId == entry.collectionId && previous.typeId == entry.typeId &&
            previous.category == entry.category && previous.name == entry.name) return;
        // A spawner can represent more than one catalogue item. Keep its proven
        // collectibility, but never choose a label by table enumeration order.
        ambiguous_.insert(spawner);
        if (previous.collectionId != entry.collectionId) previous.collectionId = 0;
        if (previous.typeId != entry.typeId) { previous.typeId = 0; previous.category.clear(); }
        if (previous.category != entry.category) previous.category.clear();
        if (previous.name != entry.name) previous.name.clear();
    }
    static void Free(const Runtime& runtime, CabbirdIl2CppGCHandleV1& handle) {
        if (handle) runtime.Functions().il2cpp_gchandle_free(handle);
        handle = nullptr;
    }
    void StopTable(const Runtime& runtime, std::string detail) {
        if (!detail_.empty()) detail_ += ';'; detail_ += detail;
        Free(runtime, enumerator_); Free(runtime, spawners_);
        if (stage_ == Stage::Collections) { entries_.clear(); ambiguous_.clear(); }
        else types_.clear();
        stage_ = stage_ == Stage::Types ? Stage::Collections : Stage::Done;
    }
    static bool FieldObject(const Runtime& runtime, Object* object, const char* name, std::string_view type, Object*& output) {
        if (!object) return false;
        const auto& api = runtime.Functions(); auto* field = runtime.FindField(api.il2cpp_object_get_class(object), name);
        if (!field || runtime.TypeName(api.il2cpp_field_get_type(field)) != type || (api.il2cpp_field_get_flags(field) & 0x10u)) return false;
        api.il2cpp_field_get_value(object, field, &output); return true;
    }
    static Object* ExistingInstance(const Runtime& runtime, Class* klass, std::string_view type, const char* fieldName = "s_Instance") {
        if (!klass) return nullptr;
        const auto& api = runtime.Functions(); auto* field = runtime.FindField(klass, fieldName);
        if (!field || runtime.TypeName(api.il2cpp_field_get_type(field)) != type || (api.il2cpp_field_get_flags(field) & 0x10u) == 0) return nullptr;
        Object* instance{}; api.il2cpp_field_static_get_value(field, &instance); return instance;
    }
    static bool MappedStream(const Runtime& runtime, Object* file) {
        if (!file) return false;
        const auto& api = runtime.Functions();
        if (runtime.TypeName(api.il2cpp_class_get_type(api.il2cpp_object_get_class(file))) !=
            "Azur.Core.MMap.LittleEndianStream") return false;
        bool memory{};
        if (!Value(runtime, Member(runtime, file, "get_readFromMemory", {}, "System.Boolean"), file, nullptr, memory)) return false;
        if (memory) return true;
        // LittleEndianStream keeps its already-open mmap view when this flag
        // is false. The flag selects copying into SimpleMemoryStream; it does
        // not distinguish an mmap view from a synchronously read FileStream.
        Object* stream{};
        if (!FieldObject(runtime, file, "m_Stream", "System.IO.Stream", stream) || !stream ||
            runtime.TypeName(api.il2cpp_class_get_type(api.il2cpp_object_get_class(stream))) !=
                "System.IO.MemoryMappedFiles.MemoryMappedViewStream") return false;
        auto* open = runtime.FindField(api.il2cpp_object_get_class(stream), "_isOpen");
        if (!open || runtime.TypeName(api.il2cpp_field_get_type(open)) != "System.Boolean" ||
            (api.il2cpp_field_get_flags(open) & 0x10u)) return false;
        bool isOpen{}; api.il2cpp_field_get_value(stream, open, &isOpen);
        return isOpen;
    }
    static bool MemoryReader(const Runtime& runtime, Object* reader) {
        if (!reader) return false;
        const auto& api = runtime.Functions();
        const auto type = runtime.TypeName(api.il2cpp_class_get_type(api.il2cpp_object_get_class(reader)));
        if (type == "Azur.Gameplay.Core.Table.DataStreamReaderXCData" || type == "Azur.Gameplay.Core.Table.DataStreamReaderTxt") return true;
        Object* file{};
        return type == "Azur.Gameplay.Core.Table.DataStreamReaderXCMMap" &&
            FieldObject(runtime, reader, "m_File", "Azur.Core.MMap.IMMapViewStream", file) && MappedStream(runtime, file);
    }
    static const Method* Static(const Runtime& runtime, Class* klass, const char* name,
        std::initializer_list<const char*> parameters, std::string_view result) {
        if (!klass) return nullptr;
        const auto* method = runtime.FindMethodBySignature(klass, name, parameters);
        std::uint32_t implementation{};
        return method && runtime.TypeName(runtime.Functions().il2cpp_method_get_return_type(method)) == result &&
            (runtime.Functions().il2cpp_method_get_flags(method, &implementation) & 0x10u) ? method : nullptr;
    }
    static Object* MappedTableReader(const Runtime& runtime, Class* manager, Object* key, bool language) {
        const auto& api = runtime.Functions();
        auto* instance = ExistingInstance(runtime, manager, "Azur.Gameplay.Core.Table.TableMgr", "p_instance");
        Object* stream{};
        if (!FieldObject(runtime, instance, "m_MMaptream", "Azur.Core.MMap.IMMapViewStream", stream) ||
            !MappedStream(runtime, stream)) return nullptr;
        // A missing per-table cache entry can still refer to the game's already
        // mapped combined table file. GetTableOffset only queries its index.
        Object* languageTag{};
        if (language) {
            const auto* currentLanguage = Static(runtime,
                runtime.FindClassAnyImage("Lens.Gameplay.Managers.I18NManager", "I18NManager"),
                "get_CurLanguageTag", {}, "System.String");
            if (!Invoke(runtime, currentLanguage, nullptr, nullptr, languageTag) || !languageTag) return nullptr;
        }
        Root languageRoot(runtime, languageTag);
        Object* offset{}; void* offsetArgs[]{key, languageRoot.get()};
        const auto* lookup = Static(runtime, manager, "GetTableOffset", {"System.String", "System.String"}, "System.Int32");
        if (!Invoke(runtime, lookup, nullptr, offsetArgs, offset) || !offset) return nullptr;
        Root offsetRoot(runtime, offset);
        const auto* value = static_cast<const std::int32_t*>(api.il2cpp_object_unbox(offsetRoot.get()));
        if (!value || *value <= 0) return nullptr;
        auto* objectClass = runtime.FindClassAnyImage("System", "Object");
        auto* readerClass = runtime.FindClassAnyImage("Azur.Gameplay.Core.Table", "DataStreamReaderXCMMap");
        if (!objectClass || !readerClass) return nullptr;
        Root objectType(runtime, api.il2cpp_type_get_object(api.il2cpp_class_get_type(objectClass)));
        Root readerType(runtime, api.il2cpp_type_get_object(api.il2cpp_class_get_type(readerClass)));
        std::int32_t length = 2; Object* arguments{}; void* arrayArgs[]{objectType.get(), &length};
        const auto* createArray = Static(runtime, runtime.FindClassAnyImage("System", "Array"), "CreateInstance",
            {"System.Type", "System.Int32"}, "System.Array");
        if (!Invoke(runtime, createArray, nullptr, arrayArgs, arguments) || !arguments) return nullptr;
        Root argumentsRoot(runtime, arguments);
        const auto* set = Member(runtime, argumentsRoot.get(), "SetValue", {"System.Object", "System.Int32"}, "System.Void");
        Object* ignored{}; std::int32_t index{}; void* first[]{stream, &index};
        if (!Invoke(runtime, set, argumentsRoot.get(), first, ignored)) return nullptr;
        index = 1; void* second[]{offsetRoot.get(), &index};
        if (!Invoke(runtime, set, argumentsRoot.get(), second, ignored)) return nullptr;
        // Reuse the native (existing mapped stream, table offset) constructor;
        // do not enter CreateDataStreamReader's cold file-loading fallback.
        const auto* create = Static(runtime, runtime.FindClassAnyImage("System", "Activator"), "CreateInstance",
            {"System.Type", "System.Object[]"}, "System.Object");
        Object* reader{}; void* args[]{readerType.get(), argumentsRoot.get()};
        if (!Invoke(runtime, create, nullptr, args, reader) || !reader) return nullptr;
        Root readerRoot(runtime, reader);
        return MemoryReader(runtime, readerRoot.get()) ? readerRoot.get() : nullptr;
    }
    static bool PreparedObject(const Runtime& runtime, Object* table, std::string_view keyType = "System.Int32", std::string* failure = nullptr) {
        const auto fail = [&](const char* reason) { if (failure) *failure = reason; return false; };
        Object *columns{}, *reader{}, *offsets{};
        if (!FieldObject(runtime, table, "m_ColOffsets", "System.Int32[]", columns) || !columns)
            return fail("m_ColOffsets unavailable");
        if (!FieldObject(runtime, table, "m_DataR", "Azur.Gameplay.Core.Table.IDataStreamReader", reader) || !MemoryReader(runtime, reader))
            return fail("m_DataR is not an open memory reader");
        bool hasIdList{};
        if (!Value(runtime, Member(runtime, reader, "get_hasIdList", {}, "System.Boolean"), reader, nullptr, hasIdList))
            return fail("IDataStreamReader.get_hasIdList failed");
        // Native TableEnumerator uses the reader's indexed ID list directly.
        // Only readers without that list require the m_DataOffset dictionary;
        // XCMMap deliberately leaves it null for both Int32 and Int64 keys.
        if (hasIdList) return true;
        const auto offsetType = "System.Collections.Generic.Dictionary<" + std::string(keyType) + ",System.Int32>";
        return (FieldObject(runtime, table, "m_DataOffset", offsetType, offsets) && offsets) ||
            fail("m_DataOffset unavailable for reader without ID list");
    }
    static bool CachedMemoryReader(const Runtime& runtime, const char* fieldName, Object* key) {
        if (!key) return false;
        const auto& api = runtime.Functions();
        auto* manager = runtime.FindClassAnyImage("Azur.Gameplay.Core.Table", "TableMgr");
        auto* field = manager ? runtime.FindField(manager, fieldName) : nullptr;
        if (!field || runtime.TypeName(api.il2cpp_field_get_type(field)) !=
            "System.Collections.Generic.Dictionary<System.String,Azur.Gameplay.Core.Table.IDataStreamReader>" ||
            (api.il2cpp_field_get_flags(field) & 0x10u) == 0) return false;
        Object* cache{}; api.il2cpp_field_static_get_value(field, &cache);
        if (!cache) return false;
        Root cacheRoot(runtime, cache);
        Object* reader{}; bool found{}; void* args[]{key, &reader};
        const auto* lookup = Member(runtime, cacheRoot.get(), "TryGetValue",
            {"System.String", "Azur.Gameplay.Core.Table.IDataStreamReader&"}, "System.Boolean");
        if (!Value(runtime, lookup, cacheRoot.get(), args, found)) return false;
        if (found) return MemoryReader(runtime, reader);
        const bool language = std::strcmp(fieldName, "m_LangTableCacheDict") == 0;
        if (!language && std::strcmp(fieldName, "m_TableCacheDict") != 0) return false;
        reader = MappedTableReader(runtime, manager, key, language);
        if (!reader) return false;
        Root readerRoot(runtime, reader); Object* ignored{};
        void* addArgs[]{key, readerRoot.get()};
        return Invoke(runtime, Member(runtime, cacheRoot.get(), "Add",
            {"System.String", "Azur.Gameplay.Core.Table.IDataStreamReader"}, "System.Void"), cacheRoot.get(), addArgs, ignored);
    }
    Object* PreparedTable(const Runtime& runtime, const char* name) {
        prepareFailure_.clear();
        const auto unavailable = [&](std::string reason) -> Object* { prepareFailure_ = std::move(reason); return nullptr; };
        const auto type = std::string("Azur.Gameplay.Table.") + name;
        auto* klass = runtime.FindClassAnyImage("Azur.Gameplay.Table", name);
        if (!klass) return unavailable("table class unavailable");
        auto* instance = ExistingInstance(runtime, klass, type);
        if (instance && PreparedObject(runtime, instance)) return instance;
        bool ready{};
        if (!Value(runtime, methods_.tableReady, nullptr, nullptr, ready) || !ready)
            return unavailable("TableMgr is not ready");
        const auto& api = runtime.Functions();
        // Exact native table keys, not asset/model name classification.
        const auto* key = std::strcmp(name, "TDWorldCollectingTypeTable") == 0 ? "world_collecting_type" : "world_collecting";
        Root keyRoot(runtime, api.il2cpp_string_new(key));
        if (!CachedMemoryReader(runtime, "m_TableCacheDict", keyRoot.get()))
            return unavailable("cached memory reader unavailable");
        // GetInstance(false) may construct metadata. Parsing is permitted only
        // after the native reader cache above proves no file-loading miss.
        const auto* get = runtime.FindMethodBySignature(klass, "GetInstance", {"System.Boolean"});
        std::uint32_t implementation{};
        if (!get || runtime.TypeName(api.il2cpp_method_get_return_type(get)) != type ||
            (api.il2cpp_method_get_flags(get, &implementation) & 0x10u) == 0)
            return unavailable("GetInstance shape mismatch");
        bool force{}; void* args[]{&force}; Object* result{};
        if (!Invoke(runtime, get, nullptr, args, result) || !result)
            return unavailable("GetInstance invocation failed");
        Root tableRoot(runtime, result); Object* iterator{};
        if (!Invoke(runtime, Member(runtime, tableRoot.get(), "GetEnumerator", {}), tableRoot.get(), nullptr, iterator) ||
            !iterator) return unavailable("GetEnumerator invocation failed");
        std::string preparedFailure;
        if (!PreparedObject(runtime, tableRoot.get(), "System.Int32", &preparedFailure))
            return unavailable("cached table initialization: " + preparedFailure);
        ++cachedTables_;
        return tableRoot.get();
    }
    bool StartEnumerator(const Runtime& runtime, Object* table, std::string_view rowType) {
        Object* object{};
        if (!Invoke(runtime, Member(runtime, table, "GetEnumerator", {}), table, nullptr, object) || !object) return false;
        Root root(runtime, object);
        const auto currentType = "System.Collections.Generic.KeyValuePair<System.Int32," + std::string(rowType) + '>';
        move_ = Member(runtime, root.get(), "MoveNext", {}, "System.Boolean");
        currentMethod_ = Member(runtime, root.get(), "get_Current", {}, currentType);
        if (!move_ || !currentMethod_) return false;
        enumerator_ = runtime.Functions().il2cpp_gchandle_new(root.get(), false);
        return enumerator_ != nullptr;
    }
    std::string RowName(const Runtime& runtime, Object* row, const Method* getter) {
        Object* lang{};
        if (!StructInvoke(runtime, getter, row, lang) || !lang) { ++missingNames_; return {}; }
        Root root(runtime, lang); Object* group{};
        if (!FieldObject(runtime, root.get(), "group", "System.String", group) || !group) { ++missingNames_; return {}; }
        const auto name = runtime.ReadString(group, 256);
        if (!name || (!readyGroups_.contains(*name) && (absentGroups_.contains(*name) || !LanguageReady(runtime, group)))) {
            if (name) absentGroups_.insert(*name); ++missingNames_; return {};
        }
        readyGroups_.insert(*name);
        Object* text{};
        if (!Invoke(runtime, methods_.langValue, root.get(), nullptr, text)) { ++missingNames_; return {}; }
        const auto value = runtime.ReadString(text, 1024);
        if (!value || value->empty()) { ++missingNames_; return {}; }
        return *value;
    }
    bool LanguageReady(const Runtime& runtime, Object* group) const {
        bool ready{};
        if (!Value(runtime, methods_.tableReady, nullptr, nullptr, ready) || !ready) return false;
        auto* manager = ExistingInstance(runtime, runtime.FindClassAnyImage("Azur.Gameplay.Core.Table", "TableMgr"), "Azur.Gameplay.Core.Table.TableMgr", "p_instance");
        Object* tables{};
        if (!manager || !FieldObject(runtime, manager, "m_LangTables", "System.Collections.Generic.Dictionary<System.String,Azur.Gameplay.Table.TDLangCommonTable>", tables) || !tables) return false;
        Object* table{}; bool found{}; void* args[]{group, &table};
        const auto* lookup = Member(runtime, tables, "TryGetValue", {"System.String", "Azur.Gameplay.Table.TDLangCommonTable&"}, "System.Boolean");
        if (!Value(runtime, lookup, tables, args, found)) return false;
        // GetLangValue creates a language-table wrapper on first use. Its
        // RefreshDataR uses this separate cache; a proven memory hit is safe.
        if (!found || !table) return CachedMemoryReader(runtime, "m_LangTableCacheDict", group);
        auto* loaded = runtime.FindField(runtime.Functions().il2cpp_object_get_class(table), "m_IsLoaded");
        if (!loaded || runtime.TypeName(runtime.Functions().il2cpp_field_get_type(loaded)) != "System.Boolean" ||
            (runtime.Functions().il2cpp_field_get_flags(loaded) & 0x10u)) return false;
        bool isLoaded{}; runtime.Functions().il2cpp_field_get_value(table, loaded, &isLoaded);
        return (isLoaded && PreparedObject(runtime, table, "System.Int64")) ||
            CachedMemoryReader(runtime, "m_LangTableCacheDict", group);
    }
    CatalogMethods methods_;
    const Method *move_{}, *currentMethod_{};
    CabbirdIl2CppGCHandleV1 enumerator_{}, spawners_{};
    CatalogEntry current_;
    std::int32_t spawnerIndex_{}, spawnerCount_{};
    std::unordered_map<std::int32_t, CatalogEntry> entries_;
    std::unordered_map<std::int32_t, std::string> types_;
    std::unordered_set<std::int32_t> ambiguous_;
    std::unordered_set<std::string> readyGroups_, absentGroups_;
    std::uint32_t typeRows_{}, collectionRows_{}, missingNames_{}, hiddenRows_{}, cachedTables_{};
    bool typesReady_{}, rowsReady_{};
    std::string detail_, prepareFailure_;
};
} // namespace collectibles
