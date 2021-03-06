//
//  src/dsc_image.c
//  tbd
//
//  Created by inoahdev on 12/29/18.
//  Copyright © 2018 - 2020 inoahdev. All rights reserved.
//

#include <unistd.h>

#include "mach-o/loader.h"
#include "mach-o/fat.h"

#include "dsc_image.h"
#include "macho_file_parse_load_commands.h"
#include "macho_file_parse_export_trie.h"
#include "macho_file_parse_symtab.h"
#include "tbd.h"

/*
 * We avoid copying code by handing most of the mach-o parsing over to the
 * macho_file namespace. To handle the different result-types, we use this
 * translate function.
 *
 * However, not all macho_file_parse_result codes are translated.
 *
 * For example, because dsc-images are not fat mach-o files, we ignore the
 * fat-related error-codes, like errors relating to architectures.
 */

static enum dsc_image_parse_result
translate_macho_file_parse_result(const enum macho_file_parse_result result) {
    switch (result) {
        case E_MACHO_FILE_PARSE_OK:
            break;

        case E_MACHO_FILE_PARSE_ERROR_PASSED_TO_CALLBACK:
            return E_DSC_IMAGE_PARSE_ERROR_PASSED_TO_CALLBACK;

        case E_MACHO_FILE_PARSE_ALLOC_FAIL:
            return E_DSC_IMAGE_PARSE_ALLOC_FAIL;

        case E_MACHO_FILE_PARSE_ARRAY_FAIL:
            return E_DSC_IMAGE_PARSE_ARRAY_FAIL;

        case E_MACHO_FILE_PARSE_SEEK_FAIL:
            return E_DSC_IMAGE_PARSE_SEEK_FAIL;

        case E_MACHO_FILE_PARSE_READ_FAIL:
            return E_DSC_IMAGE_PARSE_READ_FAIL;

        case E_MACHO_FILE_PARSE_SIZE_TOO_SMALL:
            return E_DSC_IMAGE_PARSE_SIZE_TOO_SMALL;

        case E_MACHO_FILE_PARSE_INVALID_RANGE:
            return E_DSC_IMAGE_PARSE_INVALID_RANGE;

        /*
         * Because we never call macho_file_parse_from_file(), the arch-info
         * from the header would never be checked, so this error should not be
         * returned.
         */

        case E_MACHO_FILE_PARSE_UNSUPPORTED_CPUTYPE:
            return E_DSC_IMAGE_PARSE_NOT_A_MACHO;

        /*
         * Because a dsc-image is never a fat mach-o, we will never receive the
         * following error-codes.
         */

        case E_MACHO_FILE_PARSE_NO_ARCHITECTURES:
        case E_MACHO_FILE_PARSE_TOO_MANY_ARCHITECTURES:
        case E_MACHO_FILE_PARSE_INVALID_ARCHITECTURE:
        case E_MACHO_FILE_PARSE_OVERLAPPING_ARCHITECTURES:
        case E_MACHO_FILE_PARSE_NO_VALID_ARCHITECTURES:
        case E_MACHO_FILE_PARSE_MULTIPLE_ARCHS_FOR_CPUTYPE:
        case E_MACHO_FILE_PARSE_MULTIPLE_ARCHS_FOR_PLATFORM:
            return E_DSC_IMAGE_PARSE_FAT_NOT_SUPPORTED;

        case E_MACHO_FILE_PARSE_NO_LOAD_COMMANDS:
            return E_DSC_IMAGE_PARSE_NO_LOAD_COMMANDS;

        case E_MACHO_FILE_PARSE_TOO_MANY_LOAD_COMMANDS:
            return E_DSC_IMAGE_PARSE_TOO_MANY_LOAD_COMMANDS;

        case E_MACHO_FILE_PARSE_LOAD_COMMANDS_AREA_TOO_SMALL:
            return E_DSC_IMAGE_PARSE_LOAD_COMMANDS_AREA_TOO_SMALL;

        case E_MACHO_FILE_PARSE_INVALID_LOAD_COMMAND:
            return E_DSC_IMAGE_PARSE_INVALID_LOAD_COMMAND;

        case E_MACHO_FILE_PARSE_TOO_MANY_SECTIONS:
            return E_DSC_IMAGE_PARSE_TOO_MANY_SECTIONS;

        case E_MACHO_FILE_PARSE_INVALID_SECTION:
            return E_DSC_IMAGE_PARSE_INVALID_SECTION;

        case E_MACHO_FILE_PARSE_INVALID_CLIENT:
            return E_DSC_IMAGE_PARSE_INVALID_CLIENT;

        case E_MACHO_FILE_PARSE_INVALID_REEXPORT:
            return E_DSC_IMAGE_PARSE_INVALID_REEXPORT;

        case E_MACHO_FILE_PARSE_INVALID_SYMBOL_TABLE:
            return E_DSC_IMAGE_PARSE_INVALID_SYMBOL_TABLE;

        case E_MACHO_FILE_PARSE_INVALID_STRING_TABLE:
            return E_DSC_IMAGE_PARSE_INVALID_STRING_TABLE;

        /*
         * Because a dsc-image is never a fat mach-o, we will never receive the
         * following error-code.
         */

        case E_MACHO_FILE_PARSE_CONFLICTING_ARCH_INFO:
            return E_DSC_IMAGE_PARSE_FAT_NOT_SUPPORTED;

        case E_MACHO_FILE_PARSE_NO_SYMBOL_TABLE:
            return E_DSC_IMAGE_PARSE_NO_SYMBOL_TABLE;

        case E_MACHO_FILE_PARSE_NO_DATA:
            return E_DSC_IMAGE_PARSE_NO_DATA;

        case E_MACHO_FILE_PARSE_INVALID_EXPORTS_TRIE:
            return E_DSC_IMAGE_PARSE_INVALID_EXPORTS_TRIE;

        case E_MACHO_FILE_PARSE_CREATE_SYMBOL_LIST_FAIL:
            return E_DSC_IMAGE_PARSE_CREATE_SYMBOLS_FAIL;

        case E_MACHO_FILE_PARSE_CREATE_TARGET_LIST_FAIL:
            return E_DSC_IMAGE_PARSE_CREATE_TARGET_LIST_FAIL;
    }

    return E_DSC_IMAGE_PARSE_OK;
}

/*
 * dyld_shared_cache data is stored in different mappings, with each mapping
 * copied over to memory at runtime with different memory-protections.
 *
 * To find our mach-o data, we have to take our data's memory-address, and find
 * the mapping with a memory-range containing our data's memory-address.
 *
 * The mach-o data's file-offset is simply at the mapping's file location,
 * plus the memory-mapping-index of the mach-o data.
 *
 * Some dyld_shared_cache mappings will have a memory-range larger than the
 * range reserved on file. For this reason, we may have a memory-address that
 * doesn't have a corresponding file-location.
 */

static uint64_t
get_offset_from_addr(struct dyld_shared_cache_info *__notnull const info,
                     const uint64_t address,
                     uint64_t *__notnull const max_size_out)
{
    const uint64_t count = info->mappings_count;

    const struct dyld_cache_mapping_info *mapping = info->mappings;
    const struct dyld_cache_mapping_info *const end = mapping + count;

    for (; mapping != end; mapping++) {
        const uint64_t mapping_begin = mapping->address;
        const uint64_t mapping_end = mapping_begin + mapping->size;

        const struct range mapping_range = {
            .begin = mapping_begin,
            .end = mapping_end
        };

        if (!range_contains_location(mapping_range, address)) {
            continue;
        }

        const uint64_t delta = address - mapping_begin;
        const uint64_t file_offset = mapping->fileOffset + delta;

        *max_size_out = (mapping->size - delta);
        return file_offset;
    }

    return 0;
}

static inline bool
call_callback(const macho_file_parse_error_callback callback,
              struct tbd_create_info *__notnull const info_in,
              const enum macho_file_parse_callback_type type,
              void *const cb_info)
{
    if (callback != NULL) {
        if (callback(info_in, type, cb_info)) {
            return true;
        }
    }

    return false;
}

enum dsc_image_parse_result
dsc_image_parse(struct tbd_create_info *__notnull const info_in,
                struct dyld_shared_cache_info *__notnull const dsc_info,
                struct dyld_cache_image_info *__notnull const image,
                const macho_file_parse_error_callback callback,
                void *const cb_info,
                struct string_buffer *__notnull const export_trie_sb,
                struct macho_file_parse_options macho_options,
                const struct tbd_parse_options tbd_options,
                __unused const struct dsc_image_parse_options options)
{
    uint64_t max_image_size = 0;
    const uint64_t file_offset =
        get_offset_from_addr(dsc_info, image->address, &max_image_size);

    if (file_offset == 0) {
        return E_DSC_IMAGE_PARSE_NO_MAPPING;
    }

    if (max_image_size < sizeof(struct mach_header)) {
        return E_DSC_IMAGE_PARSE_SIZE_TOO_SMALL;
    }

    const uint8_t *const map = dsc_info->map;
    const struct mach_header *const header =
        (const struct mach_header *)(map + file_offset);

    const uint32_t magic = header->magic;
    struct macho_file_parse_lc_flags lc_flags = {};

    const bool is_64 = (magic == MH_MAGIC_64 || magic == MH_CIGAM_64);
    const bool is_big_endian = (magic == MH_CIGAM || magic == MH_CIGAM_64);

    if (is_64) {
        if (max_image_size < sizeof(struct mach_header_64)) {
            return E_DSC_IMAGE_PARSE_SIZE_TOO_SMALL;
        }

        if (is_big_endian) {
            lc_flags.is_big_endian = true;
        }

        lc_flags.is_64 = true;
    } else {
        const bool is_fat =
            magic == FAT_MAGIC || magic == FAT_MAGIC_64 ||
            magic == FAT_CIGAM || magic == FAT_CIGAM_64;

        if (is_fat) {
            return E_DSC_IMAGE_PARSE_FAT_NOT_SUPPORTED;
        }

        if (!is_big_endian) {
            if (magic != MH_MAGIC) {
                return E_DSC_IMAGE_PARSE_NOT_A_MACHO;
            }
        } else {
            lc_flags.is_big_endian = true;
        }
    }

    const uint32_t flags = header->flags;
    if (flags & MH_TWOLEVEL) {
        info_in->fields.flags.flat_namespace = true;
    }

    if (!(flags & MH_APP_EXTENSION_SAFE)) {
        info_in->fields.flags.not_app_extension_safe = true;
    }

    if (flags & MH_NLIST_OUTOFSYNC_WITH_DYLDINFO) {
        if (macho_options.use_symbol_table) {
            const bool should_continue =
                call_callback(callback,
                              info_in,
                              WARN_MACHO_FILE_SYMBOL_TABLE_OUTOFSYNC,
                              cb_info);

            if (!should_continue) {
                return E_DSC_IMAGE_PARSE_ERROR_PASSED_TO_CALLBACK;
            }
        }
    }

    if (flags & MH_SIM_SUPPORT) {
        if (dsc_info->flags.has_simulator_header) {
            if (!dsc_info->flags.is_simulator) {
                return E_DSC_IMAGE_PARSE_SIMULATOR_TYPE_MISMATCH;
            }
        }

        lc_flags.expecting_sim_platform = true;
    }

    info_in->flags.uses_full_targets = true;

    /*
     * The symbol-table and string-table's file-offsets are relative to the
     * cache-base, not the mach-o header. However, all other mach-o information
     * we parse is relative to the mach-o header.
     *
     * Because of this conundrum, we use the flags below to parse the symbol and
     * string tables separately.
     */

    macho_options.dont_parse_exports = true;
    macho_options.sect_off_absolute = true;

    const uint32_t header_size =
        (is_64) ? sizeof(struct mach_header_64) : sizeof(struct mach_header);

    struct mf_parse_lc_from_map_info info = {
        .map = map,
        .map_size = dsc_info->size,

        .macho = (const uint8_t *)header,
        .macho_size = max_image_size,

        .arch = dsc_info->arch,
        .available_map_range = dsc_info->available_range,

        .ncmds = header->ncmds,
        .sizeofcmds = header->sizeofcmds,
        .header_size = header_size,

        .tbd_options = tbd_options,
        .options = macho_options,

        .flags = lc_flags
    };

    struct macho_file_parse_extra_args extra = {
        .callback = callback,
        .cb_info = cb_info,
        .export_trie_sb = export_trie_sb
    };

    struct macho_file_lc_info_out lc_info = {};
    const enum macho_file_parse_result parse_load_commands_result =
        macho_file_parse_load_commands_from_map(info_in,
                                                &info,
                                                extra,
                                                &lc_info);

    if (parse_load_commands_result != E_MACHO_FILE_PARSE_OK) {
        return translate_macho_file_parse_result(parse_load_commands_result);
    }

    bool parsed_dyld_info = false;
    bool parse_symtab = true;

    /*
     * Parse dyld_info if available, and if not, parse symtab instead. However,
     * if private.
     */

    enum macho_file_parse_result ret = E_MACHO_FILE_PARSE_OK;
    if (!macho_options.use_symbol_table) {
        if (lc_info.export_off != 0 && lc_info.export_size != 0) {
            const struct macho_file_parse_export_trie_args args = {
                .info_in = info_in,
                .available_range = dsc_info->available_range,

                .is_64 = is_64,
                .is_big_endian = is_big_endian,

                .export_off = lc_info.export_off,
                .export_size = lc_info.export_size,

                .sb_buffer = extra.export_trie_sb,
                .tbd_options = tbd_options
            };

            ret = macho_file_parse_export_trie_from_map(args, map);
            if (ret != E_MACHO_FILE_PARSE_OK) {
                return translate_macho_file_parse_result(ret);
            }

            parsed_dyld_info = true;
            if (lc_info.symtab.nsyms != 0) {
                parse_symtab = !tbd_options.ignore_undefineds;
            } else {
                parse_symtab = false;
            }
        } else if (lc_info.symtab.nsyms == 0) {
            return E_DSC_IMAGE_PARSE_NO_SYMBOL_TABLE;
        }
    } else if (lc_info.symtab.nsyms == 0) {
        return E_DSC_IMAGE_PARSE_NO_SYMBOL_TABLE;
    }

    if (parse_symtab) {
        const struct macho_file_parse_symtab_args args = {
            .info_in = info_in,
            .available_range = dsc_info->available_range,

            .is_big_endian = is_big_endian,

            .symoff = lc_info.symtab.symoff,
            .nsyms = lc_info.symtab.nsyms,

            .stroff = lc_info.symtab.stroff,
            .strsize = lc_info.symtab.strsize,

            .tbd_options = tbd_options
        };

        if (is_64) {
            ret = macho_file_parse_symtab_64_from_map(&args, map);
        } else {
            ret = macho_file_parse_symtab_from_map(&args, map);
        }
    } else if (!parsed_dyld_info) {
        /*
         * If we have either ignore_exports, or ignore_missing_exports, we don't
         * have an error.
         */

        if (tbd_options.ignore_exports || tbd_options.ignore_missing_exports) {
            return E_DSC_IMAGE_PARSE_OK;
        }

        return E_DSC_IMAGE_PARSE_NO_DATA;
    }

    if (ret != E_MACHO_FILE_PARSE_OK) {
        return translate_macho_file_parse_result(ret);
    }

    return E_DSC_IMAGE_PARSE_OK;
}
