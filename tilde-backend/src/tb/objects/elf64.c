#include "elf64.h"

struct TB_ModuleExporter {
    size_t write_pos;

    size_t temporary_memory_capacity;
    void* temporary_memory;
};

static void put_symbol(TB_Emitter* strtbl, TB_Emitter* stab, const char* name, uint8_t sym_info, Elf64_Half section_index, Elf64_Addr value, Elf64_Xword size) {
    // Fill up the symbol's string table
    size_t name_len = strlen(name);
    size_t name_pos = strtbl->count;

    tb_out_reserve(strtbl, name_len + 1);
    tb_outs_UNSAFE(strtbl, name_len + 1, (uint8_t*)name);

    // Emit symbol
    Elf64_Sym sym = {
        .st_name  = name_pos,
        .st_info  = sym_info,
        .st_shndx = section_index,
        .st_value = value,
        .st_size  = size
    };

    tb_out_reserve(stab, sizeof(Elf64_Sym));
    tb_outs_UNSAFE(stab, sizeof(Elf64_Sym), (uint8_t*)&sym);
}

#define WRITE(data, length_) write_data(&e, output, length_, data)
static void write_data(TB_ModuleExporter* restrict e, uint8_t* restrict output, size_t length, const void* data) {
    memcpy(output + e->write_pos, data, length);
    e->write_pos += length;
}

static void zero_data(TB_ModuleExporter* restrict e, uint8_t* restrict output, size_t length) {
    memset(output + e->write_pos, 0, length);
    e->write_pos += length;
}

static void write_text_section(TB_ModuleExporter* restrict e, TB_Module* m, uint8_t* output) {
    FOREACH_N(i, 0, m->functions.count) {
        TB_FunctionOutput* out_f = m->functions.data[i].output;
        if (out_f != NULL) {
            write_data(e, output, out_f->code_size, out_f->code);
        }
    }
}

static void write_data_section(TB_ModuleExporter* restrict e, TB_Module* m, uint8_t* output, uint32_t pos) {
    assert(e->write_pos == pos);

    uint8_t* data = &output[pos];
    e->write_pos += m->data_region_size;

    FOREACH_N(i, 0, m->max_threads) {
        pool_for(TB_Global, g, m->thread_info[i].globals) {
            if (g->storage != TB_STORAGE_DATA) continue;

            TB_Initializer* init = g->init;

            // clear out space
            memset(&data[g->pos], 0, init->size);

            FOREACH_N(k, 0, init->obj_count) {
                if (init->objects[k].type == TB_INIT_OBJ_REGION) {
                    memcpy(&data[g->pos + init->objects[k].offset], init->objects[k].region.ptr, init->objects[k].region.size);
                }
            }
        }
    }
}

static void write_rodata_section(TB_ModuleExporter* e, TB_Module* m, uint8_t* output, uint32_t pos) {
    assert(e->write_pos == pos);

    uint8_t* rdata = &output[pos];
    e->write_pos += m->rdata_region_size;

    FOREACH_N(i, 0, m->max_threads) {
        FOREACH_N(j, 0, dyn_array_length(m->thread_info[i].const_patches)) {
            TB_ConstPoolPatch* p = &m->thread_info[i].const_patches[j];
            memcpy(&rdata[p->rdata_pos], p->data, p->length);
        }
    }
}

static void* get_temporary_storage(TB_ModuleExporter* e, size_t request_size) {
    if (e->temporary_memory_capacity < request_size) {
        e->temporary_memory_capacity = tb_next_pow2(request_size);
        if (e->temporary_memory_capacity < (4*1024*1024)) {
            e->temporary_memory_capacity = (4*1024*1024);
        }

        e->temporary_memory = tb_platform_heap_realloc(e->temporary_memory, e->temporary_memory_capacity);
    }

    return e->temporary_memory;
}

// returns array of m->functions.count+1 where the last slot is the size of the text section
static uint32_t* get_text_section_layout(TB_Module* m) {
    uint32_t* func_layout = tb_platform_heap_alloc((m->functions.count + 1) * sizeof(uint32_t));

    size_t offset = 0;
    FOREACH_N(i, 0, m->functions.count) {
        TB_FunctionOutput* out_f = m->functions.data[i].output;

        func_layout[i] = offset;
        if (out_f) offset += out_f->code_size;
    }

    func_layout[m->functions.count] = offset;
    return func_layout;
}

TB_API TB_Exports tb_elf64obj_write_output(TB_Module* m, const IDebugFormat* dbg) {
    // used by the sections array
    enum {
        S_NULL,
        S_STRTAB,
        S_TEXT,
        S_TEXT_REL,
        S_DATA,
        S_RODATA,
        S_BSS,
        S_STAB,
        S_MAX
    };

    TB_ModuleExporter e = { 0 };

    // tally up .data relocations
    /*uint32_t data_relocation_count = 0;

    FOREACH_N(t, 0, m->max_threads) {
        pool_for(TB_Global, g, m->thread_info[t].globals) {
            TB_Initializer* init = g->init;
            FOREACH_N(k, 0, init->obj_count) {
                data_relocation_count += (init->objects[k].type != TB_INIT_OBJ_REGION);
            }
        }
    }*/

    // mark each with a unique id
    uint32_t function_sym_start = S_MAX;
    uint32_t external_symbol_baseline = function_sym_start + m->functions.compiled_count;

    size_t unique_id_counter = 0;
    FOREACH_N(i, 0, m->max_threads) {
        pool_for(TB_External, ext, m->thread_info[i].externals) {
            int id = external_symbol_baseline + unique_id_counter;
            ext->address = (void*) (uintptr_t) id;
            unique_id_counter += 1;
        }

        pool_for(TB_Global, g, m->thread_info[i].globals) {
            g->id = external_symbol_baseline + unique_id_counter;
            unique_id_counter += 1;
        }
    }

    uint16_t machine = 0;
    switch (m->target_arch) {
        case TB_ARCH_X86_64: machine = EM_X86_64; break;
        case TB_ARCH_AARCH64: machine = EM_AARCH64; break;
        default: tb_todo();
    }

    Elf64_Ehdr header = {
        .e_ident = {
            [EI_MAG0]       = 0x7F, // magic number
            [EI_MAG1]       = 'E',
            [EI_MAG2]       = 'L',
            [EI_MAG3]       = 'F',
            [EI_CLASS]      = 2, // 64bit ELF file
            [EI_DATA]       = 1, // little-endian
            [EI_VERSION]    = 1, // 1.0
            [EI_OSABI]      = 0,
            [EI_ABIVERSION] = 0
        },
        .e_type = ET_REL, // relocatable
        .e_version = 1,
        .e_machine = machine,
        .e_entry = 0,

        // section headers go at the end of the file
        // and are filed in later.
        .e_shoff = 0,
        .e_flags = 0,

        .e_ehsize = sizeof(Elf64_Ehdr),

        .e_shentsize = sizeof(Elf64_Shdr),
        .e_shnum     = S_MAX,
        .e_shstrndx  = 1
    };

    Elf64_Shdr sections[S_MAX] = {
        [S_STRTAB] = {
            .sh_type = SHT_STRTAB,
            .sh_flags = 0,
            .sh_addralign = 1
        },
        [S_TEXT] = {
            .sh_type = SHT_PROGBITS,
            .sh_flags = SHF_EXECINSTR | SHF_ALLOC,
            .sh_addralign = 16
        },
        [S_TEXT_REL] = {
            .sh_type = SHT_RELA,
            .sh_flags = SHF_INFO_LINK,
            .sh_link = 7,
            .sh_info = 2,
            .sh_addralign = 16,
            .sh_entsize = sizeof(Elf64_Rela)
        },
        [S_DATA] = {
            .sh_type = SHT_PROGBITS,
            .sh_flags = SHF_ALLOC | SHF_WRITE,
            .sh_addralign = 16
        },
        [S_RODATA] = {
            .sh_type = SHT_PROGBITS,
            .sh_flags = SHF_ALLOC,
            .sh_addralign = 16
        },
        [S_BSS] = {
            .sh_type = SHT_NOBITS,
            .sh_flags = SHF_ALLOC | SHF_WRITE,
            .sh_addralign = 16
        },
        [S_STAB] = {
            .sh_type = SHT_SYMTAB,
            .sh_flags = 0, .sh_addralign = 1,
            .sh_link = 1, .sh_info = header.e_shnum,
            .sh_entsize = sizeof(Elf64_Sym)
        }
    };

    const ICodeGen* restrict code_gen = tb__find_code_generator(m);
    static const char* SECTION_NAMES[] = {
        NULL, ".strtab", ".text", ".rela.text", ".data", ".rodata", ".bss", ".symtab"
    };

    // Section string table:
    TB_Emitter strtbl = { 0 };
    {
        tb_out_reserve(&strtbl, 1024);
        tb_out1b(&strtbl, 0); // null string in the table
        FOREACH_N(i, 1, S_MAX) {
            sections[i].sh_name = tb_outstr_nul_UNSAFE(&strtbl, SECTION_NAMES[i]);
        }
    }

    // Code section
    // [m->functions.count + 1] last slot is the size of the text section
    uint32_t* func_layout = tb_platform_heap_alloc((m->functions.count + 1) * sizeof(uint32_t));
    {

        size_t offset = 0;
        FOREACH_N(i, 0, m->functions.count) {
            TB_FunctionOutput* out_f = m->functions.data[i].output;

            func_layout[i] = offset;
            if (out_f) offset += out_f->code_size;
        }
        func_layout[m->functions.count] = offset;
        sections[S_TEXT].sh_size = offset;

        // Target specific: resolve internal call patches
        code_gen->emit_call_patches(m, func_layout);
    }

    FOREACH_N(i, 0, m->max_threads) {
        sections[S_TEXT_REL].sh_size += dyn_array_length(m->thread_info[i].ecall_patches) * sizeof(Elf64_Rela);
        sections[S_TEXT_REL].sh_size += dyn_array_length(m->thread_info[i].const_patches) * sizeof(Elf64_Rela);
    }

    // write symbol table
    TB_Emitter stab = { 0 };

    // NULL symbol
    tb_out_zero(&stab, sizeof(Elf64_Sym));

    FOREACH_N(i, 1, S_MAX) {
        put_symbol(&strtbl, &stab, SECTION_NAMES[i], ELF64_ST_INFO(ELF64_STB_LOCAL, 3), i, 0, 0);
    }

    FOREACH_N(i, 0, m->functions.count) {
        TB_FunctionOutput* out_f = m->functions.data[i].output;
        if (!out_f) continue;

        // calculate size
        size_t func_size = func_layout[i + 1] - func_layout[i];
        put_symbol(&strtbl, &stab, m->functions.data[i].name, ELF64_ST_INFO(ELF64_STB_GLOBAL, 2), 2, func_layout[i], func_size);
    }

    FOREACH_N(i, 0, m->max_threads) {
        pool_for(TB_External, external, m->thread_info[i].externals) {
            put_symbol(&strtbl, &stab, external->name, ELF64_ST_INFO(ELF64_STB_GLOBAL, 0), 0, 0, 0);
        }
    }

    // set some sizes and pass the stab and string table to the context
    sections[S_STAB].sh_size   = stab.count;
    sections[S_STRTAB].sh_size = strtbl.count;
    sections[S_DATA].sh_size   = m->data_region_size;
    sections[S_RODATA].sh_size = m->rdata_region_size;

    // Calculate file offsets
    size_t output_size = sizeof(Elf64_Ehdr);
    FOREACH_N(i, 0, S_MAX) {
        sections[i].sh_offset = output_size;
        output_size += sections[i].sh_size;
    }

    // section headers
    header.e_shoff = output_size;
    output_size += S_MAX * sizeof(Elf64_Shdr);

    // Allocate memory now
    uint8_t* restrict output = tb_platform_heap_alloc(output_size);

    // Write contents
    {
        WRITE(&header, sizeof(Elf64_Ehdr));
        WRITE(strtbl.data, strtbl.count);

        // TEXT section
        assert(e.write_pos == sections[S_TEXT].sh_offset);
        write_text_section(&e, m, output);

        // TEXT patches
        {
            assert(e.write_pos == sections[S_TEXT_REL].sh_offset);

            TB_FIXED_ARRAY(Elf64_Rela) relocs = {
                .cap = sections[S_TEXT_REL].sh_size / sizeof(Elf64_Rela),
                .elems = (Elf64_Rela*) &output[sections[S_TEXT_REL].sh_offset]
            };

            FOREACH_N(i, 0, m->max_threads) {
                FOREACH_N(j, 0, dyn_array_length(m->thread_info[i].ecall_patches)) {
                    TB_ExternFunctionPatch* p = &m->thread_info[i].ecall_patches[j];
                    TB_FunctionOutput* out_f = p->source->output;

                    size_t actual_pos = func_layout[p->source - m->functions.data] +
                        out_f->prologue_length + p->pos;

                    int symbol_id = external_symbol_baseline + (uintptr_t) p->target->address;
                    Elf64_Rela rela = {
                        .r_offset = actual_pos,
                        .r_info   = ELF64_R_INFO(symbol_id, R_X86_64_PLT32),
                        .r_addend = -4
                    };
                    TB_FIXED_ARRAY_APPEND(relocs, rela);
                }

                FOREACH_N(j, 0, dyn_array_length(m->thread_info[i].const_patches)) {
                    TB_ConstPoolPatch* p = &m->thread_info[i].const_patches[j];
                    TB_FunctionOutput* out_f = p->source->output;

                    size_t actual_pos = func_layout[p->source - m->functions.data] +
                        out_f->prologue_length + p->pos;

                    Elf64_Rela rela = {
                        .r_offset = actual_pos,
                        .r_info   = ELF64_R_INFO(S_RODATA, R_X86_64_PLT32),
                        .r_addend = -4
                    };
                    TB_FIXED_ARRAY_APPEND(relocs, rela);
                }
            }

            WRITE(relocs.elems, relocs.count * sizeof(Elf64_Rela));
        }

        write_data_section(&e, m, output, sections[S_DATA].sh_offset);
        write_rodata_section(&e, m, output, sections[S_RODATA].sh_offset);

        assert(e.write_pos == sections[S_STAB].sh_offset);
        WRITE(stab.data, stab.count);

        assert(e.write_pos == header.e_shoff);
        WRITE(sections, S_MAX * sizeof(Elf64_Shdr));
    }

    // Done
    tb_platform_heap_free(strtbl.data);
    tb_platform_heap_free(stab.data);
    tb_platform_heap_free(func_layout);

    return (TB_Exports){ .count = 1, .files = { { output_size, output } } };
}

TB_API TB_Exports tb_elf64exe_write_output(TB_Module* m, const IDebugFormat* dbg) {
    enum {
        S_TEXT,
        S_RODATA,
        S_MAX,
    };

    TB_ModuleExporter e = { 0 };

    uint16_t machine = 0;
    switch (m->target_arch) {
        case TB_ARCH_X86_64: machine = EM_X86_64; break;
        case TB_ARCH_AARCH64: machine = EM_AARCH64; break;
        default: tb_todo();
    }

    Elf64_Ehdr header = {
        .e_ident = {
            [EI_MAG0]       = 0x7F, // magic number
            [EI_MAG1]       = 'E',
            [EI_MAG2]       = 'L',
            [EI_MAG3]       = 'F',
            [EI_CLASS]      = 2, // 64bit ELF file
            [EI_DATA]       = 1, // little-endian
            [EI_VERSION]    = 1, // 1.0
            [EI_OSABI]      = 0,
            [EI_ABIVERSION] = 0
        },
        .e_type = ET_EXEC,
        .e_version = 1,
        .e_machine = machine,
        .e_entry = 0,

        .e_flags = 0,
        .e_ehsize = sizeof(Elf64_Ehdr),

        // segment headers go at the end of the file
        // and are filed in later.
        .e_phoff     = 0,
        .e_phentsize = sizeof(Elf64_Phdr),
        .e_phnum     = S_MAX,
    };

    Elf64_Phdr sections[] = {
        [S_TEXT] = {
            .p_type = PT_LOAD,
            .p_flags = PF_X | PF_R,
            .p_align = 4096,
        },
        [S_RODATA] = {
            .p_type = PT_LOAD,
            .p_flags = PF_R,
            .p_align = 4096,

            .p_memsz = m->rdata_region_size,
            .p_filesz = m->rdata_region_size
        }
    };

    // Code section
    // [m->functions.count + 1] last slot is the size of the text section
    uint32_t* func_layout = get_text_section_layout(m);
    {
        size_t code_section_size = func_layout[m->functions.count];

        // memory size is aligned to 4K bytes because of page alignment
        sections[S_TEXT].p_memsz = align_up(code_section_size, sections[S_TEXT].p_align);
        sections[S_TEXT].p_filesz = code_section_size;
    }

    // Layout sections in virtual memory
    {
        size_t offset = sizeof(Elf64_Ehdr);
        FOREACH_N(i, 0, S_MAX) {
            sections[i].p_vaddr = offset;
            offset = align_up(offset + sections[i].p_memsz, sections[i].p_align);
        }
    }

    // Apply TEXT relocations
    {
        // Target specific: resolve internal call patches
        const ICodeGen* restrict code_gen = tb__find_code_generator(m);
        code_gen->emit_call_patches(m, func_layout);

        // TODO: Handle rodata relocations
        FOREACH_N(i, 0, m->max_threads) {
            FOREACH_N(j, 0, dyn_array_length(m->thread_info[i].ecall_patches)) {
                //TB_ExternFunctionPatch* p = &m->thread_info[i].ecall_patches[j];
                //TB_FunctionOutput* out_f = p->source->output;
                tb_todo();
            }

            FOREACH_N(j, 0, dyn_array_length(m->thread_info[i].const_patches)) {
                TB_ConstPoolPatch* p = &m->thread_info[i].const_patches[j];
                TB_FunctionOutput* out_f = p->source->output;
                assert(out_f && "Patch cannot be applied to function with no compiled output");

                size_t actual_pos = func_layout[p->source - m->functions.data] +
                    out_f->prologue_length + p->pos + 4;

                uint32_t* patch_mem = (uint32_t*) &out_f->code[out_f->prologue_length + p->pos];
                *patch_mem += sections[S_RODATA].p_vaddr - actual_pos;
            }
        }
    }

    // Layout sections in file memory
    size_t file_offset = sizeof(Elf64_Ehdr);
    FOREACH_N(i, 0, S_MAX) {
        file_offset = align_up(file_offset, 4096);
        sections[i].p_offset = file_offset;

        file_offset += sections[i].p_filesz;
    }
    header.e_phoff = file_offset;

    size_t output_size = file_offset + (S_MAX * sizeof(Elf64_Phdr));
    uint8_t* output = tb_platform_heap_alloc(output_size);

    {
        WRITE(&header, sizeof(Elf64_Ehdr));

        zero_data(&e, output, align_up(e.write_pos, 4096) - e.write_pos);
        write_text_section(&e, m, output);

        zero_data(&e, output, align_up(e.write_pos, 4096) - e.write_pos);
        write_rodata_section(&e, m, output, sections[S_RODATA].p_offset);

        WRITE(sections, S_MAX * sizeof(Elf64_Phdr));
    }

    return (TB_Exports){ .count = 1, .files = { { output_size, output } } };
}