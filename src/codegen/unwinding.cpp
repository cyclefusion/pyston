// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/unwinding.h"

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include "llvm/DebugInfo/DIContext.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Object/ObjectFile.h"

#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/stackmaps.h"
#include "runtime/types.h"


#define UNW_LOCAL_ONLY
#include <libunwind.h>

// Definition from libunwind, but standardized I suppose by the format of the .eh_frame_hdr section:
struct uw_table_entry {
    int32_t start_ip_offset;
    int32_t fde_offset;
};

namespace pyston {

// Parse an .eh_frame section, and construct a "binary search table" such as you would find in a .eh_frame_hdr section.
// Currently only supports .eh_frame sections with exactly one fde.
void parseEhFrame(uint64_t start_addr, uint64_t size, uint64_t* out_data, uint64_t* out_len) {
    union {
        uint8_t* u8;
        uint32_t* u32;
    };
    u32 = (uint32_t*)start_addr;

    int cie_length = *u32;
    *u32++;

    assert(*u32 == 0); // CIE ID

    u8 += cie_length;

    int fde_length = *u32;
    u32++;

    assert(cie_length + fde_length + 8 == size && "more than one fde! (supportable, but not implemented)");

    int nentries = 1;
    uw_table_entry* table_data = new uw_table_entry[nentries];
    table_data->start_ip_offset = 0;
    table_data->fde_offset = 4 + cie_length;

    *out_data = (uintptr_t)table_data;
    *out_len = nentries;
}

class CFRegistry {
private:
    // TODO use a binary search tree
    std::vector<CompiledFunction*> cfs;

public:
    void registerCF(CompiledFunction* cf) { cfs.push_back(cf); }

    // addr is the return address of the callsite, so we will check it against
    // the region (start, end] (opposite-endedness of normal half-open regions)
    CompiledFunction* getCFForAddress(uint64_t addr) {
        for (auto* cf : cfs) {
            if (cf->code_start < addr && addr <= cf->code_start + cf->code_size)
                return cf;
        }
        return NULL;
    }
};

static CFRegistry cf_registry;

CompiledFunction* getCFForAddress(uint64_t addr) {
    return cf_registry.getCFForAddress(addr);
}

class TracebacksEventListener : public llvm::JITEventListener {
public:
    virtual void NotifyObjectEmitted(const llvm::object::ObjectFile& Obj,
                                     const llvm::RuntimeDyld::LoadedObjectInfo& L) {
        std::unique_ptr<llvm::DIContext> Context(llvm::DIContext::getDWARFContext(Obj));

        assert(g.cur_cf);

        llvm_error_code ec;
        for (const auto& sym : Obj.symbols()) {
            llvm::object::SymbolRef::Type SymType;
            if (sym.getType(SymType))
                continue;
            if (SymType == llvm::object::SymbolRef::ST_Function) {
                llvm::StringRef Name;
                uint64_t Addr;
                uint64_t Size;
                if (sym.getName(Name))
                    continue;
                Addr = L.getSymbolLoadAddress(Name);
                assert(Addr);
                if (sym.getSize(Size))
                    continue;

// TODO this should be the Python name, not the C name:
#if LLVMREV < 208921
                llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(
                    Addr, Size, llvm::DILineInfoSpecifier::FunctionName | llvm::DILineInfoSpecifier::FileLineInfo
                                    | llvm::DILineInfoSpecifier::AbsoluteFilePath);
#else
                llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(
                    Addr, Size, llvm::DILineInfoSpecifier(llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                                                          llvm::DILineInfoSpecifier::FunctionNameKind::LinkageName));
#endif
                if (VERBOSITY() >= 2) {
                    for (int i = 0; i < lines.size(); i++) {
                        printf("%s:%d, %s: %lx\n", lines[i].second.FileName.c_str(), lines[i].second.Line,
                               lines[i].second.FunctionName.c_str(), lines[i].first);
                    }
                }

                assert(g.cur_cf->code_start == 0);
                g.cur_cf->code_start = Addr;
                g.cur_cf->code_size = Size;
                cf_registry.registerCF(g.cur_cf);
            }
        }

        // Currently-unused libunwind support:
        llvm_error_code code;
        bool found_text = false, found_eh_frame = false;
        uint64_t text_addr = -1, text_size = -1;
        uint64_t eh_frame_addr = -1, eh_frame_size = -1;

        for (const auto& sec : Obj.sections()) {
            llvm::StringRef name;
            code = sec.getName(name);
            assert(!code);

            uint64_t addr, size;
            if (name == ".eh_frame") {
                assert(!found_eh_frame);
                eh_frame_addr = L.getSectionLoadAddress(name);
                eh_frame_size = sec.getSize();

                if (VERBOSITY())
                    printf("eh_frame: %lx %lx\n", eh_frame_addr, eh_frame_size);
                found_eh_frame = true;
            } else if (name == ".text") {
                assert(!found_text);
                text_addr = L.getSectionLoadAddress(name);
                text_size = sec.getSize();

                if (VERBOSITY())
                    printf("text: %lx %lx\n", text_addr, text_size);
                found_text = true;
            }
        }

        assert(found_text);
        assert(found_eh_frame);

        unw_dyn_info_t* dyn_info = new unw_dyn_info_t();
        dyn_info->start_ip = text_addr;
        dyn_info->end_ip = text_addr + text_size;
        dyn_info->format = UNW_INFO_FORMAT_REMOTE_TABLE;

        dyn_info->u.rti.name_ptr = 0;
        dyn_info->u.rti.segbase = eh_frame_addr;
        parseEhFrame(eh_frame_addr, eh_frame_size, &dyn_info->u.rti.table_data, &dyn_info->u.rti.table_len);

        if (VERBOSITY())
            printf("dyn_info = %p, table_data = %p\n", dyn_info, (void*)dyn_info->u.rti.table_data);
        _U_dyn_register(dyn_info);

        // TODO: it looks like libunwind does a linear search over anything dynamically registered,
        // as opposed to the binary search it can do within a dyn_info.
        // If we're registering a lot of dyn_info's, it might make sense to coalesce them into a single
        // dyn_info that contains a binary search table.
    }
};

struct PythonFrameId {
    enum FrameType {
        COMPILED,
        INTERPRETED,
    } type;

    union {
        uint64_t ip; // if type == COMPILED
        uint64_t bp; // if type == INTERPRETED
    };
};

class PythonFrameIterator {
private:
    PythonFrameId id;

    unw_context_t ctx;
    unw_cursor_t cursor;
    CompiledFunction* cf;
    bool cur_is_osr;

    PythonFrameIterator() : cf(NULL), cur_is_osr(false) {}

    // not copyable or movable, since 'cursor' holds an internal pointer to 'ctx'
    PythonFrameIterator(const PythonFrameIterator&) = delete;
    void operator=(const PythonFrameIterator&) = delete;
    PythonFrameIterator(const PythonFrameIterator&&) = delete;
    void operator=(const PythonFrameIterator&&) = delete;

public:
    CompiledFunction* getCF() const {
        assert(cf);
        return cf;
    }

    uint64_t readLocation(const StackMap::Record::Location& loc) {
        if (loc.type == StackMap::Record::Location::LocationType::Register) {
            // TODO: need to make sure we deal with patchpoints appropriately
            return getReg(loc.regnum);
        } else if (loc.type == StackMap::Record::Location::LocationType::Direct) {
            uint64_t reg_val = getReg(loc.regnum);
            return reg_val + loc.offset;
        } else if (loc.type == StackMap::Record::Location::LocationType::Indirect) {
            uint64_t reg_val = getReg(loc.regnum);
            uint64_t addr = reg_val + loc.offset;
            return *reinterpret_cast<uint64_t*>(addr);
        } else if (loc.type == StackMap::Record::Location::LocationType::Constant) {
            return loc.offset;
        } else if (loc.type == StackMap::Record::Location::LocationType::ConstIndex) {
            int const_idx = loc.offset;
            assert(const_idx >= 0);
            assert(const_idx < cf->location_map->constants.size());
            return getCF()->location_map->constants[const_idx];
        } else {
            printf("%d %d %d %d\n", loc.type, loc.flags, loc.regnum, loc.offset);
            abort();
        }
    }

    AST_stmt* getCurrentStatement() {
        if (id.type == PythonFrameId::COMPILED) {
            CompiledFunction* cf = getCF();
            uint64_t ip = getId().ip;

            assert(ip > cf->code_start);
            unsigned offset = ip - cf->code_start;

            assert(cf->location_map);
            const LocationMap::LocationTable& table = cf->location_map->names["!current_stmt"];
            assert(table.locations.size());

            // printf("Looking for something at offset %d (total ip: %lx)\n", offset, ip);
            for (const LocationMap::LocationTable::LocationEntry& e : table.locations) {
                // printf("(%d, %d]\n", e.offset, e.offset + e.length);
                if (e.offset < offset && offset <= e.offset + e.length) {
                    // printf("Found it\n");
                    assert(e.locations.size() == 1);
                    return reinterpret_cast<AST_stmt*>(readLocation(e.locations[0]));
                }
            }
            abort();
        } else if (id.type == PythonFrameId::INTERPRETED) {
            return getCurrentStatementForInterpretedFrame((void*)id.bp);
        }
        abort();
    }

    FrameInfo* getFrameInfo() {
        if (id.type == PythonFrameId::COMPILED) {
            CompiledFunction* cf = getCF();
            assert(cf->location_map->frameInfoFound());
            const auto& frame_info_loc = cf->location_map->frame_info_location;

            return reinterpret_cast<FrameInfo*>(readLocation(frame_info_loc));
        } else if (id.type == PythonFrameId::INTERPRETED) {
            return getFrameInfoForInterpretedFrame((void*)id.bp);
        }
        abort();
    }

    const PythonFrameId& getId() const { return id; }

    static std::unique_ptr<PythonFrameIterator> end() { return std::unique_ptr<PythonFrameIterator>(nullptr); }

    static std::unique_ptr<PythonFrameIterator> begin() {
        std::unique_ptr<PythonFrameIterator> rtn(new PythonFrameIterator());

        unw_getcontext(&rtn->ctx);
        unw_init_local(&rtn->cursor, &rtn->ctx);

        bool found = rtn->incr();
        if (!found)
            return NULL;
        return rtn;
    }

    uint64_t getReg(int dwarf_num) {
        assert(0 <= dwarf_num && dwarf_num < 16);

        // for x86_64, at least, libunwind seems to use the dwarf numbering

        unw_word_t rtn;
        int code = unw_get_reg(&cursor, dwarf_num, &rtn);
        assert(code == 0);
        return rtn;
    }

    bool incr() {
        bool was_osr = cur_is_osr;

        while (true) {
            int r = unw_step(&this->cursor);

            if (r <= 0) {
                return false;
            }

            unw_word_t ip;
            unw_get_reg(&this->cursor, UNW_REG_IP, &ip);

            cf = getCFForAddress(ip);
            if (cf) {
                this->id.type = PythonFrameId::COMPILED;
                this->id.ip = ip;

                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                cur_is_osr = (bool)cf->entry_descriptor;
                if (was_osr) {
                    // Skip the frame we just found if the previous one was its OSR
                    // TODO this will break if we start collapsing the OSR frames
                    return incr();
                }

                return true;
            }

            // TODO shouldn't need to do this expensive-looking query, if we
            // knew the bounds of the interpretFunction() function:
            unw_proc_info_t pip;
            int code = unw_get_proc_info(&this->cursor, &pip);
            RELEASE_ASSERT(code == 0, "%d", code);

            if (pip.start_ip == (intptr_t)interpreter_instr_addr) {
                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                this->id.type = PythonFrameId::INTERPRETED;
                this->id.bp = bp;
                cf = getCFForInterpretedFrame((void*)bp);

                cur_is_osr = (bool)cf->entry_descriptor;
                if (was_osr) {
                    // Skip the frame we just found if the previous one was its OSR
                    // TODO this will break if we start collapsing the OSR frames
                    return incr();
                }

                return true;
            }

            // keep unwinding
        }
    }

    // Adapter classes to be able to use this in a C++11 range for loop.
    // (Needed because we do more memory management than typical.)
    // TODO: maybe the memory management should be handled by the Manager?
    class Manager {
    public:
        class Holder {
        public:
            std::unique_ptr<PythonFrameIterator> it;

            Holder(std::unique_ptr<PythonFrameIterator> it) : it(std::move(it)) {}

            bool operator!=(const Holder& rhs) const {
                assert(rhs.it.get() == NULL); // this is the only intended use case, for comparing to end()
                return it != rhs.it;
            }

            Holder& operator++() {
                assert(it.get());

                bool found = it->incr();
                if (!found)
                    it.release();
                return *this;
            }

            PythonFrameIterator& operator*() const {
                assert(it.get());
                return *it.get();
            }
        };

        Holder begin() { return PythonFrameIterator::begin(); }

        Holder end() { return PythonFrameIterator::end(); }
    };
};

PythonFrameIterator::Manager unwindPythonFrames() {
    return PythonFrameIterator::Manager();
}

static std::unique_ptr<PythonFrameIterator> getTopPythonFrame() {
    std::unique_ptr<PythonFrameIterator> fr = PythonFrameIterator::begin();
    RELEASE_ASSERT(fr != PythonFrameIterator::end(), "no valid python frames??");

    return fr;
}

static const LineInfo* lineInfoForFrame(PythonFrameIterator& frame_it) {
    AST_stmt* current_stmt = frame_it.getCurrentStatement();
    auto* cf = frame_it.getCF();
    assert(cf);
    return new LineInfo(current_stmt->lineno, current_stmt->col_offset, cf->clfunc->source->parent_module->fn,
                        cf->clfunc->source->getName());
}

std::vector<const LineInfo*> getTracebackEntries() {
    std::vector<const LineInfo*> entries;

    if (!ENABLE_FRAME_INTROSPECTION) {
        static bool printed_warning = false;
        if (!printed_warning) {
            printed_warning = true;
            fprintf(stderr, "Warning: can't get traceback since ENABLE_FRAME_INTROSPECTION=0\n");
        }
        return entries;
    }

    for (auto& frame_info : unwindPythonFrames()) {
        const LineInfo* line_info = lineInfoForFrame(frame_info);
        if (line_info)
            entries.push_back(line_info);
    }

    std::reverse(entries.begin(), entries.end());
    return entries;
}

const LineInfo* getMostRecentLineInfo() {
    std::unique_ptr<PythonFrameIterator> frame = getTopPythonFrame();
    return lineInfoForFrame(*frame);
}

ExcInfo* getFrameExcInfo() {
    std::vector<ExcInfo*> to_update;
    ExcInfo* copy_from_exc = NULL;
    ExcInfo* cur_exc = NULL;
    for (PythonFrameIterator& frame_iter : unwindPythonFrames()) {
        FrameInfo* frame_info = frame_iter.getFrameInfo();

        copy_from_exc = &frame_info->exc;
        if (!cur_exc)
            cur_exc = copy_from_exc;

        if (!copy_from_exc->type) {
            to_update.push_back(copy_from_exc);
            continue;
        }

        break;
    }

    assert(copy_from_exc); // Only way this could still be NULL is if there weren't any python frames

    if (!copy_from_exc->type) {
        // No exceptions found:
        *copy_from_exc = ExcInfo(None, None, None);
    }

    assert(copy_from_exc->value);
    assert(copy_from_exc->traceback);

    for (auto* ex : to_update) {
        *ex = *copy_from_exc;
    }
    assert(cur_exc);
    return cur_exc;
}

CompiledFunction* getTopCompiledFunction() {
    return getTopPythonFrame()->getCF();
}

BoxedModule* getCurrentModule() {
    CompiledFunction* compiledFunction = getTopCompiledFunction();
    assert(compiledFunction);
    return compiledFunction->clfunc->source->parent_module;
}

BoxedDict* getLocals(bool only_user_visible) {
    for (PythonFrameIterator& frame_info : unwindPythonFrames()) {
        if (frame_info.getId().type == PythonFrameId::COMPILED) {
            BoxedDict* d = new BoxedDict();

            CompiledFunction* cf = frame_info.getCF();
            uint64_t ip = frame_info.getId().ip;

            assert(ip > cf->code_start);
            unsigned offset = ip - cf->code_start;

            assert(cf->location_map);
            for (const auto& p : cf->location_map->names) {
                if (only_user_visible && (p.first[0] == '#' || p.first[0] == '!'))
                    continue;

                for (const LocationMap::LocationTable::LocationEntry& e : p.second.locations) {
                    if (e.offset < offset && offset <= e.offset + e.length) {
                        const auto& locs = e.locations;

                        llvm::SmallVector<uint64_t, 1> vals;
                        // printf("%s: %s\n", p.first.c_str(), e.type->debugName().c_str());

                        for (auto& loc : locs) {
                            vals.push_back(frame_info.readLocation(loc));
                        }

                        Box* v = e.type->deserializeFromFrame(vals);
                        // printf("%s: (pp id %ld) %p\n", p.first.c_str(), e._debug_pp_id, v);
                        assert(gc::isValidGCObject(v));
                        d->d[boxString(p.first)] = v;
                    }
                }
            }

            return d;
        } else if (frame_info.getId().type == PythonFrameId::INTERPRETED) {
            return localsForInterpretedFrame((void*)frame_info.getId().bp, only_user_visible);
        } else {
            abort();
        }
    }
    RELEASE_ASSERT(0, "Internal error: unable to find any python frames");
}


llvm::JITEventListener* makeTracebacksListener() {
    return new TracebacksEventListener();
}
}
