#include "app_internal.h"

/* Installed-content resolution plus ExeFS and RomFS access. */

/* NCM content lookup ----------------------------------------------------- */

Result open_data_storage_by_path(FsStorage *out, const char *path,
                                 FsContentAttributes attributes,
                                 u32 proxy_type) {
    const struct {
        u8 attributes;
        u8 padding[3];
        u32 filesystem_type;
    } input = {
        attributes,
        {0},
        proxy_type,
    };
    char send_path[FS_MAX_PATH] = {0};
    size_t path_length = strnlen(path, sizeof(send_path));
    if (path_length >= sizeof(send_path))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memcpy(send_path, path, path_length + 1);
    Service *fsp = fsGetServiceSession();
    serviceAssumeDomain(fsp);
    return serviceDispatchIn(fsp, 206, input,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { send_path, sizeof(send_path) } },
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

static Result get_content_attributes(NcmContentMetaDatabase *database,
                                     const NcmContentMetaKey *key,
                                     const NcmContentId *content_id,
                                     u8 content_type, u8 *out_attributes) {
    NcmContentInfo infos[32] = {0};
    s32 written = 0;
    Result result = ncmContentMetaDatabaseListContentInfo(
        database, &written, infos, 32, key, 0);
    if (R_FAILED(result)) return result;
    for (s32 i = 0; i < written; ++i) {
        if (infos[i].content_type == content_type &&
            !memcmp(&infos[i].content_id, content_id, sizeof(*content_id))) {
            *out_attributes = infos[i].attr & 0x0f;
            return 0;
        }
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

/* Titles such as Switch 2 Editions carry more than one Program (the original
   game at index 0 and a native program at a higher index). When set, every
   Program lookup below selects exactly this index instead of the platform's
   preferred one. -1 restores the automatic choice. */
static int g_program_index_override = -1;

void set_program_index_override(int program_index) {
    g_program_index_override = program_index;
    log_line("program-index-override index=%d", program_index);
}

static Result select_installed_content(
    NcmContentMetaDatabase *database, const NcmContentMetaKey *key,
    u8 content_type, NcmContentId *out_content_id, u8 *out_attributes,
    u8 *out_platform, u8 *out_id_offset) {
    *out_attributes = 0;
    *out_platform = 0xff;
    *out_id_offset = 0;

    if (content_type == NcmContentType_Program &&
        g_program_index_override >= 0) {
        NcmContentInfo infos[32] = {0};
        s32 written = 0;
        Result list_result = ncmContentMetaDatabaseListContentInfo(
            database, &written, infos, 32, key, 0);
        if (R_SUCCEEDED(list_result)) {
            list_result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
            for (s32 i = 0; i < written; ++i) {
                if (infos[i].content_type != content_type ||
                    infos[i].id_offset != g_program_index_override)
                    continue;
                *out_content_id = infos[i].content_id;
                *out_attributes = infos[i].attr & 0x0f;
                *out_id_offset = infos[i].id_offset;
                list_result = 0;
                break;
            }
        }
        log_line("selected-content-variant id=%016llX forced-id-offset=%d attributes=%u result=0x%08x",
                 (unsigned long long)key->id, g_program_index_override,
                 *out_attributes, list_result);
        return list_result;
    }

    Result platform_result = ncmContentMetaDatabaseGetPlatform(
        database, out_platform, key);
    if (R_FAILED(platform_result)) {
        log_line("selected-content-platform id=%016llX result=0x%08x",
                 (unsigned long long)key->id, platform_result);
    }

    /*
     * Switch 2 content metadata can contain both the original Program and a
     * native Program.  Its platform value matches the native Program's ID
     * offset, while the ordinary GetContentIdByType call always returns
     * offset zero.
     */
    if (content_type == NcmContentType_Program &&
        R_SUCCEEDED(platform_result)) {
        NcmContentInfo infos[32] = {0};
        s32 written = 0;
        Result list_result = ncmContentMetaDatabaseListContentInfo(
            database, &written, infos, 32, key, 0);
        if (R_SUCCEEDED(list_result)) {
            s32 first = -1;
            s32 matching_platform = -1;
            for (s32 i = 0; i < written; ++i) {
                if (infos[i].content_type != content_type) continue;
                if (first < 0) first = i;
                if (infos[i].id_offset == *out_platform) {
                    matching_platform = i;
                    break;
                }
            }

            s32 selected = matching_platform >= 0
                ? matching_platform : first;
            if (selected >= 0) {
                *out_content_id = infos[selected].content_id;
                *out_attributes = infos[selected].attr & 0x0f;
                *out_id_offset = infos[selected].id_offset;
                log_line("selected-content-variant id=%016llX platform=%u id-offset=%u attributes=%u candidates=%d match=%u",
                         (unsigned long long)key->id, *out_platform,
                         *out_id_offset, *out_attributes, written,
                         matching_platform >= 0);
                return 0;
            }
        } else {
            log_line("selected-content-list id=%016llX result=0x%08x",
                     (unsigned long long)key->id, list_result);
        }
    }

    Result result = ncmContentMetaDatabaseGetContentIdByType(
        database, out_content_id, key, content_type);
    if (R_SUCCEEDED(result))
        result = get_content_attributes(database, key, out_content_id,
                                        content_type, out_attributes);
    log_line("selected-content-variant id=%016llX platform=%u id-offset=%u attributes=%u fallback=1 result=0x%08x",
             (unsigned long long)key->id, *out_platform, *out_id_offset,
             *out_attributes, result);
    return result;
}

static void bytes_to_hex(char *output, size_t output_size,
                         const void *input, size_t input_size) {
    const u8 *bytes = input;
    if (!output_size) return;
    size_t at = 0;
    for (size_t i = 0; i < input_size && at + 2 < output_size; ++i) {
        int written = snprintf(output + at, output_size - at, "%02X", bytes[i]);
        if (written != 2) break;
        at += 2;
    }
    output[at] = 0;
}

static Result resolve_location_path(Service *resolver, u32 command, u64 id,
                                    char *path, u8 attributes[16]) {
    struct {
        u8 attributes[16];
    } output = {{0}};
    Result result = serviceDispatchInOut(resolver, command, id, output,
        .buffer_attrs = {
            SfBufferAttr_HipcPointer | SfBufferAttr_Out |
            SfBufferAttr_FixedSize,
        },
        .buffers = { { path, FS_MAX_PATH } },
    );
    if (R_SUCCEEDED(result))
        memcpy(attributes, output.attributes, sizeof(output.attributes));
    return result;
}

static void log_location_resolvers(const char *label, u8 storage_id,
                                   u64 application_id, u64 runtime_id) {
    Result result = lrInitialize();
    log_result("exefs-diagnostic-lr-initialize", result);
    if (R_FAILED(result)) return;

    LrLocationResolver resolver = {0};
    result = lrOpenLocationResolver((NcmStorageId)storage_id, &resolver);
    log_line("exefs-diagnostic-lr-open label=%s storage=%u result=0x%08x",
             label, storage_id, result);
    if (R_SUCCEEDED(result)) {
        const u64 ids[] = {application_id, runtime_id};
        const char *kinds[] = {"application", "runtime"};
        for (size_t i = 0; i < 2; ++i) {
            if (i && ids[i] == ids[0]) continue;
            char path[FS_MAX_PATH] = {0};
            u8 path_attributes[16] = {0};
            Result path_result = resolve_location_path(
                &resolver.s, 0, ids[i], path, path_attributes);
            char attributes_hex[sizeof(path_attributes) * 2 + 1];
            bytes_to_hex(attributes_hex, sizeof(attributes_hex),
                         path_attributes, sizeof(path_attributes));
            log_line("exefs-diagnostic-lr-path label=%s kind=%s id=%016llX result=0x%08x attributes=%s path=%s",
                     label, kinds[i], (unsigned long long)ids[i], path_result,
                     attributes_hex,
                     R_SUCCEEDED(path_result) ? path : "(unavailable)");

            memset(path, 0, sizeof(path));
            memset(path_attributes, 0, sizeof(path_attributes));
            path_result = resolve_location_path(
                &resolver.s, 16, ids[i], path, path_attributes);
            bytes_to_hex(attributes_hex, sizeof(attributes_hex),
                         path_attributes, sizeof(path_attributes));
            log_line("exefs-diagnostic-lr-debug-path label=%s kind=%s id=%016llX result=0x%08x attributes=%s path=%s",
                     label, kinds[i], (unsigned long long)ids[i], path_result,
                     attributes_hex,
                     R_SUCCEEDED(path_result) ? path : "(unavailable)");
        }
        serviceClose(&resolver.s);
    }

    LrRegisteredLocationResolver registered = {0};
    result = lrOpenRegisteredLocationResolver(&registered);
    log_line("exefs-diagnostic-lr-open-registered label=%s result=0x%08x",
             label, result);
    if (R_SUCCEEDED(result)) {
        const u64 ids[] = {application_id, runtime_id};
        const char *kinds[] = {"application", "runtime"};
        for (size_t i = 0; i < 2; ++i) {
            if (i && ids[i] == ids[0]) continue;
            char path[FS_MAX_PATH] = {0};
            u8 path_attributes[16] = {0};
            Result path_result = resolve_location_path(
                &registered.s, 0, ids[i], path, path_attributes);
            char attributes_hex[sizeof(path_attributes) * 2 + 1];
            bytes_to_hex(attributes_hex, sizeof(attributes_hex),
                         path_attributes, sizeof(path_attributes));
            log_line("exefs-diagnostic-lr-registered-path label=%s kind=%s id=%016llX result=0x%08x attributes=%s path=%s",
                     label, kinds[i], (unsigned long long)ids[i], path_result,
                     attributes_hex,
                     R_SUCCEEDED(path_result) ? path : "(unavailable)");
        }
        serviceClose(&registered.s);
    }
    lrExit();
}

static void log_program_content_diagnostics(
    const char *label, const NsApplicationContentMetaStatus *status) {
    if (!status) {
        log_line("exefs-diagnostic-content label=%s status=absent", label);
        return;
    }

    log_line("exefs-diagnostic-content-begin label=%s id=%016llX meta-type=%u storage=%u rights-check=%u reserved=%u version=%u",
             label, (unsigned long long)status->application_id,
             status->meta_type, status->storageID, status->rights_check,
             status->reserved, status->version);

    Result result = ncmInitialize();
    log_result("exefs-diagnostic-ncm-initialize", result);
    if (R_FAILED(result)) return;

    NcmContentMetaKey key = {
        .id = status->application_id,
        .version = status->version,
        .type = status->meta_type,
        .install_type = NcmContentInstallType_Full,
    };
    NcmContentMetaDatabase database = {0};
    result = ncmOpenContentMetaDatabase(&database, status->storageID);
    log_line("exefs-diagnostic-db-open label=%s storage=%u result=0x%08x",
             label, status->storageID, result);

    NcmContentInfo infos[64] = {0};
    s32 written = 0;
    if (R_SUCCEEDED(result)) {
        u8 platform = 0xff;
        Result platform_result = ncmContentMetaDatabaseGetPlatform(
            &database, &platform, &key);
        log_line("exefs-diagnostic-platform label=%s result=0x%08x platform=%u",
                 label, platform_result, platform);

        u8 meta_attributes = 0;
        Result attributes_result = ncmContentMetaDatabaseGetAttributes(
            &database, &key, &meta_attributes);
        log_line("exefs-diagnostic-meta-attributes label=%s result=0x%08x attributes=%u",
                 label, attributes_result, meta_attributes);

        result = ncmContentMetaDatabaseListContentInfo(
            &database, &written, infos, 64, &key, 0);
        log_line("exefs-diagnostic-content-list label=%s result=0x%08x count=%d",
                 label, result, written);
        ncmContentMetaDatabaseClose(&database);
    }

    NcmContentStorage storage = {0};
    Result storage_result = R_SUCCEEDED(result)
        ? ncmOpenContentStorage(&storage, status->storageID) : result;
    log_line("exefs-diagnostic-storage-open label=%s storage=%u result=0x%08x",
             label, status->storageID, storage_result);

    if (R_SUCCEEDED(result)) {
        for (s32 i = 0; i < written; ++i) {
            NcmContentInfo *info = &infos[i];
            char content_id[sizeof(info->content_id) * 2 + 1];
            bytes_to_hex(content_id, sizeof(content_id),
                         &info->content_id, sizeof(info->content_id));
            u64 content_size = 0;
            ncmContentInfoSizeToU64(info, &content_size);
            log_line("exefs-diagnostic-content-info label=%s index=%d id=%s type=%u id-offset=%u attributes-raw=%u attributes=%u size=%llu",
                     label, i, content_id, info->content_type, info->id_offset,
                     info->attr, info->attr & 0x0f,
                     (unsigned long long)content_size);
            if (info->content_type != NcmContentType_Program ||
                R_FAILED(storage_result)) continue;

            char path[FS_MAX_PATH] = {0};
            Result path_result = ncmContentStorageGetPath(
                &storage, path, sizeof(path), &info->content_id);
            log_line("exefs-diagnostic-program-path label=%s id-offset=%u result=0x%08x path=%s",
                     label, info->id_offset, path_result,
                     R_SUCCEEDED(path_result) ? path : "(unavailable)");
            if (R_FAILED(path_result)) continue;

            FsContentAttributes attributes =
                (FsContentAttributes)(info->attr & 0x0f);
            u64 ncm_program_id = 0;
            Result ncm_program_result = ncmContentStorageGetProgramId(
                &storage, &ncm_program_id, &info->content_id, attributes);
            u64 fs_program_id = 0;
            Result fs_program_result = fsGetProgramId(
                &fs_program_id, path, attributes);
            log_line("exefs-diagnostic-program-id label=%s id-offset=%u ncm-result=0x%08x ncm=%016llX fs-result=0x%08x fs=%016llX application=%016llX",
                     label, info->id_offset, ncm_program_result,
                     (unsigned long long)ncm_program_id, fs_program_result,
                     (unsigned long long)fs_program_id,
                     (unsigned long long)status->application_id);

            NcmRightsId ncm_rights = {0};
            Result ncm_rights_result =
                ncmContentStorageGetRightsIdFromContentId(
                    &storage, &ncm_rights, &info->content_id, attributes);
            char ncm_rights_id[sizeof(ncm_rights.rights_id) * 2 + 1];
            bytes_to_hex(ncm_rights_id, sizeof(ncm_rights_id),
                         &ncm_rights.rights_id,
                         sizeof(ncm_rights.rights_id));
            log_line("exefs-diagnostic-ncm-rights label=%s result=0x%08x generation=%u id=%s",
                     label, ncm_rights_result, ncm_rights.key_generation,
                     ncm_rights_id);

            FsRightsId fs_rights = {0};
            u8 fs_generation = 0;
            Result fs_rights_result = fsGetRightsIdAndKeyGenerationByPath(
                path, attributes, &fs_generation, &fs_rights);
            char fs_rights_id[sizeof(fs_rights) * 2 + 1];
            bytes_to_hex(fs_rights_id, sizeof(fs_rights_id),
                         &fs_rights, sizeof(fs_rights));
            log_line("exefs-diagnostic-fs-rights label=%s result=0x%08x generation=%u id=%s",
                     label, fs_rights_result, fs_generation, fs_rights_id);

            u64 runtime_id = R_SUCCEEDED(ncm_program_result)
                ? ncm_program_id : fs_program_id;
            if (!runtime_id) runtime_id = status->application_id;
            log_location_resolvers(
                label, status->storageID, status->application_id, runtime_id);
        }
    }

    if (R_SUCCEEDED(storage_result)) ncmContentStorageClose(&storage);
    ncmExit();
    log_line("exefs-diagnostic-content-end label=%s", label);
}

/* Export rights ---------------------------------------------------------- */

static bool rights_id_is_zero(const FsRightsId *rights_id) {
    const u8 *bytes = (const u8 *)rights_id;
    for (size_t i = 0; i < sizeof(*rights_id); ++i)
        if (bytes[i]) return false;
    return true;
}

static Result dispatch_license_prepare(Service *es, const FsRightsId *rights_id,
                                       u32 generation) {
    return serviceDispatch(es, 503,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { rights_id, sizeof(*rights_id) },
            { &generation, sizeof(generation) },
        },
    );
}

static bool license_needs_export_context(Result result) {
    const u32 canonical = result & 0x3fffff;
    return canonical == 0x1491 || canonical == 0x691;
}

/* ES treats the first eight rights-id bytes as a big-endian title id. For
   an update or DLC that is the update/DLC id, not the owning application. */
static u64 rights_title_id(const FsRightsId *rights_id) {
    const u8 *rights = (const u8 *)rights_id;
    u64 title_id = 0;
    for (size_t i = 0; i < 8; ++i) title_id = (title_id << 8) | rights[i];
    return title_id;
}

static Result prepare_export_rights_context(Service *context, Service *es,
                                            u64 title_id) {
    Result result = 0;
    if (!serviceIsActive(context)) {
        result = serviceDispatch(es, 2001,
            .out_num_objects = 1,
            .out_objects = context,
        );
        log_result("license-context-create", result);
        if (R_FAILED(result)) return result;
    }

    /* The export context accepts the title id in little-endian form. */
    u8 rights_application[8] = {0};
    for (size_t i = 0; i < sizeof(rights_application); ++i)
        rights_application[i] = (u8)(title_id >> (8 * i));

    result = serviceDispatch(context, 2,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { rights_application, sizeof(rights_application) } },
    );
    log_result("license-context-select-title", result);
    if (R_FAILED(result)) return result;

    const u8 request_all_accounts = 1;
    u64 empty_account = 0;
    result = serviceDispatchIn(context, 17, request_all_accounts,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { rights_application, sizeof(rights_application) },
            { &empty_account, sizeof(empty_account) },
        },
    );
    log_result("license-context-request-accounts", result);
    if (R_FAILED(result)) return result;

    result = serviceDispatch(context, 3);
    log_result("license-context-evaluate", result);
    return result;
}

static Result prepare_installed_license(NcmContentStorage *storage,
                                        const NcmContentId *content_id,
                                        FsContentAttributes attributes) {
    NcmRightsId rights = {0};
    Result result = ncmContentStorageGetRightsIdFromContentId(
        storage, &rights, content_id, attributes);
    const u8 *id = (const u8 *)&rights.rights_id;
    log_line("license-rights result=0x%08x generation=%u present=%u id=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             result, rights.key_generation,
             !rights_id_is_zero(&rights.rights_id),
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
             id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
    if (R_FAILED(result) || rights_id_is_zero(&rights.rights_id)) return result;

    Service es = {0};
    result = smGetService(&es, "es");
    if (R_SUCCEEDED(result)) {
        u32 generation = rights.key_generation;
        result = dispatch_license_prepare(&es, &rights.rights_id, generation);
        log_result("license-prepare-initial", result);
        if (license_needs_export_context(result)) {
            /* First select the title named by the rights id (correct for
               digital base games). For updates and DLC that is not the
               owning application, so retry once with the application id in
               a separate context, leaving the first context untouched. */
            const u64 rights_title = rights_title_id(&rights.rights_id);
            const u64 application_id = g.count > 0
                ? g.applications[g.selected].record.application_id : 0;
            struct {
                Service *context;
                u64 title_id;
                const char *kind;
            } attempts[] = {
                { &g_license_context, rights_title, "rights-title" },
                { &g_license_application_context, application_id,
                  "application" },
            };
            const size_t attempt_count =
                application_id && application_id != rights_title ? 2 : 1;
            for (size_t i = 0; i < attempt_count; ++i) {
                log_line("license-context-attempt kind=%s title=%016llX",
                         attempts[i].kind,
                         (unsigned long long)attempts[i].title_id);
                Result context_result = prepare_export_rights_context(
                    attempts[i].context, &es, attempts[i].title_id);
                if (R_FAILED(context_result)) {
                    result = context_result;
                    continue;
                }
                result = dispatch_license_prepare(
                    &es, &rights.rights_id, generation);
                log_result("license-prepare-after-context", result);
                if (R_SUCCEEDED(result)) {
                    Result activate_result = serviceDispatch(
                        attempts[i].context, 102);
                    log_result("license-context-activate", activate_result);
                    if (R_FAILED(activate_result)) result = activate_result;
                    break;
                }
                if (!license_needs_export_context(result)) break;
            }
        }
        serviceClose(&es);
    }
    log_result("license-prepare", result);
    return result;
}

/* Control metadata ------------------------------------------------------- */

void title_from_nacp(Application *app, NacpStruct *nacp) {
    if (nacp->titles_data_format == 1) {
        const size_t output_size = 32 * sizeof(NacpLanguageEntry);
        NacpLanguageEntry *entries = calloc(1, output_size);
        if (entries) {
            z_stream stream = {0};
            stream.next_in = nacp->lang_data.compressed_data.buffer;
            stream.avail_in = nacp->lang_data.compressed_data.buffer_size;
            stream.next_out = (Bytef *)entries;
            stream.avail_out = output_size;
            int zresult = inflateInit2(&stream, -15);
            if (zresult == Z_OK) zresult = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            log_line("control-inflate title=%016llX zlib=%d input=%u output=%lu",
                     (unsigned long long)app->record.application_id, zresult,
                     nacp->lang_data.compressed_data.buffer_size,
                     (unsigned long)stream.total_out);
            if (zresult == Z_STREAM_END) {
                for (size_t i = 0; i < 32; ++i) {
                    if (!entries[i].name[0]) continue;
                    copy_display_text(app->title, sizeof(app->title), entries[i].name);
                    break;
                }
            }
            free(entries);
        }
    } else {
        NacpLanguageEntry *language = NULL;
        if (R_FAILED(nacpGetLanguageEntry(nacp, &language))) language = NULL;
        if (language)
            copy_display_text(app->title, sizeof(app->title), language->name);
    }
    copy_display_text(app->version, sizeof(app->version), nacp->display_version);
}

Result load_control_from_content(Application *app) {
    free(g.control_icon);
    g.control_icon = NULL;
    g.control_icon_size = 0;
    NsApplicationContentMetaStatus statuses[16] = {0};
    s32 count = 0;
    Result result = nsListApplicationContentMetaStatus(
        app->record.application_id, 0, statuses, 16, &count);
    log_line("control-status-list title=%016llX result=0x%08x count=%d",
             (unsigned long long)app->record.application_id, result, count);
    if (R_FAILED(result)) return result;
    if (count > 16) count = 16;
    for (s32 i = 0; i < count; ++i) {
        log_line("control-status title=%016llX index=%d status-id=%016llX type=%u storage=%u version=%u",
                 (unsigned long long)app->record.application_id, i,
                 (unsigned long long)statuses[i].application_id,
                 statuses[i].meta_type, statuses[i].storageID,
                 statuses[i].version);
    }

    /* NS can expose only a non-base status for some installed legacy games.
       Add direct base-key probes so NCM remains the source of truth. */
    const u8 fallback_storages[] = {
        NcmStorageId_BuiltInUser,
        NcmStorageId_SdCard,
        NcmStorageId_GameCard,
    };
    for (size_t i = 0;
         i < sizeof(fallback_storages) && count < 16; ++i) {
        statuses[count].meta_type = NcmContentMetaType_Application;
        statuses[count].storageID = fallback_storages[i];
        statuses[count].application_id = app->record.application_id;
        ++count;
    }

    result = ncmInitialize();
    if (R_FAILED(result)) return result;
    Result last = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    const u8 wanted_types[] = {
        NcmContentMetaType_Patch,
        NcmContentMetaType_Application,
    };
    for (size_t wanted = 0; wanted < sizeof(wanted_types); ++wanted) {
      for (s32 i = 0; i < count; ++i) {
        NsApplicationContentMetaStatus *status = &statuses[i];
        if (status->meta_type != wanted_types[wanted]) continue;
        NcmContentMetaDatabase database = {0};
        NcmContentStorage storage = {0};
        NcmContentId content_id = {0};
        NcmContentMetaKey key = {
            .id = status->application_id,
            .version = status->version,
            .type = status->meta_type,
            .install_type = NcmContentInstallType_Full,
        };
        u8 content_attributes = 0;
        char path[FS_MAX_PATH] = {0};
        last = ncmOpenContentMetaDatabase(&database, status->storageID);
        if (R_SUCCEEDED(last)) {
            /* Preserve the actual installed key, including install_type and
               platform-era fields, instead of reconstructing it from NS. */
            last = ncmContentMetaDatabaseGetLatestContentMetaKey(
                &database, &key, status->application_id);
            log_line("control-meta-key title=%016llX status-id=%016llX type=%u storage=%u result=0x%08x key-type=%u install=%u version=%u",
                     (unsigned long long)app->record.application_id,
                     (unsigned long long)status->application_id,
                     status->meta_type, status->storageID, last, key.type,
                     key.install_type, key.version);
            if (R_SUCCEEDED(last))
                last = ncmContentMetaDatabaseGetContentIdByType(
                    &database, &content_id, &key, NcmContentType_Control);
            if (R_SUCCEEDED(last))
                last = get_content_attributes(&database, &key, &content_id,
                                              NcmContentType_Control,
                                              &content_attributes);
            ncmContentMetaDatabaseClose(&database);
        }
        if (R_FAILED(last)) continue;
        last = ncmOpenContentStorage(&storage, status->storageID);
        if (R_SUCCEEDED(last)) {
            last = ncmContentStorageGetPath(&storage, path, sizeof(path), &content_id);
            ncmContentStorageClose(&storage);
        }
        log_line("control-path title=%016llX result=0x%08x path=%s",
                 (unsigned long long)app->record.application_id, last,
                 R_SUCCEEDED(last) ? path : "(unavailable)");
        if (R_FAILED(last)) continue;

        FsStorage data_storage = {0};
        last = open_data_storage_by_path(
            &data_storage, path, (FsContentAttributes)content_attributes, 3);
        log_line("control-open-storage title=%016llX attributes=%u result=0x%08x",
                 (unsigned long long)app->record.application_id,
                 content_attributes, last);
        if (R_FAILED(last)) continue;
        last = romfsMountFromStorage(data_storage, 0, "kdctl");
        if (R_FAILED(last)) continue;

        FILE *file = fopen("kdctl:/control.nacp", "rb");
        if (!file) {
            last = MAKERESULT(Module_Libnx, LibnxError_IoError);
        } else {
            memset(&g.control_data.nacp, 0, sizeof(g.control_data.nacp));
            size_t bytes = fread(&g.control_data.nacp, 1,
                                 sizeof(g.control_data.nacp), file);
            fclose(file);
            if (bytes == sizeof(g.control_data.nacp)) {
                title_from_nacp(app, &g.control_data.nacp);
                last = app->title[0] ? 0 : MAKERESULT(Module_Libnx, LibnxError_NotFound);
            } else {
                last = MAKERESULT(Module_Libnx, LibnxError_IoError);
            }
        }
        if (R_SUCCEEDED(last))
            g.control_icon_size = load_control_icon();
        romfsUnmount("kdctl");
        if (R_SUCCEEDED(last)) break;
      }
      if (R_SUCCEEDED(last)) break;
    }
    ncmExit();
    return last;
}

/* RomFS ------------------------------------------------------------------ */

static bool result_is_missing_external_key(Result result);

static int romfs_failure_rank(Result result) {
    if (R_SUCCEEDED(result)) return -1;
    const u32 value = R_VALUE(result);
    if (value == 0x7d802) return 3;
    if (value == 0x7d202) return 2;
    if (value == 0x2ee202 ||
        value == R_VALUE(MAKERESULT(Module_Libnx, LibnxError_NotFound)))
        return 0;
    return 1;
}

Result prefer_romfs_failure(Result kept, Result candidate) {
    return romfs_failure_rank(candidate) > romfs_failure_rank(kept)
        ? candidate : kept;
}

static Result mount_explicit_program_content(u64 application_id,
                                             bool *romfs_device) {
    Result result = ncmInitialize();
    log_result("ncmInitialize", result);
    if (R_FAILED(result)) return result;

    /* An update's RomFS is a delta on top of the base RomFS. Opened on its
       own it can even mount, but every file that comes from the base then
       fails to read (EIO), so this route only uses the base, and reports
       that the update is missing. Merged RomFS needs the routes above. */
    const u8 wanted_types[] = {
        NcmContentMetaType_Application,
    };
    Result last = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    Result reported = 0;
    bool update_failed =
        find_program_status(NcmContentMetaType_Patch) != NULL;
    if (update_failed)
        log_line("romfs-explicit-skip-update reason=update-romfs-needs-base");
    for (size_t wanted = 0; wanted < sizeof(wanted_types); ++wanted) {
        for (s32 i = 0; i < g.meta_count; ++i) {
            NsApplicationContentMetaStatus *status = &g.meta[i];
            if (status->meta_type != wanted_types[wanted]) continue;
            const bool is_update =
                status->meta_type == NcmContentMetaType_Patch;

            NcmContentMetaDatabase database = {0};
            NcmContentStorage storage = {0};
            NcmContentId content_id = {0};
            NcmContentMetaKey key = {
                .id = status->application_id,
                .version = status->version,
                .type = status->meta_type,
                .install_type = NcmContentInstallType_Full,
            };
            u8 content_attributes = 0;
            char path[FS_MAX_PATH] = {0};

            last = ncmOpenContentMetaDatabase(&database, status->storageID);
            log_line("ncm-open-db type=%u storage=%u result=0x%08x",
                     status->meta_type, status->storageID, last);
            if (R_SUCCEEDED(last)) {
                last = ncmContentMetaDatabaseGetContentIdByType(
                    &database, &content_id, &key, NcmContentType_Program);
                if (R_SUCCEEDED(last))
                    last = get_content_attributes(&database, &key, &content_id,
                                                  NcmContentType_Program,
                                                  &content_attributes);
                log_line("ncm-program-id type=%u version=%u result=0x%08x",
                         status->meta_type, status->version, last);
                ncmContentMetaDatabaseClose(&database);
            }
            if (R_FAILED(last)) {
                reported = prefer_romfs_failure(reported, last);
                if (is_update) update_failed = true;
                continue;
            }

            last = ncmOpenContentStorage(&storage, status->storageID);
            log_line("ncm-open-storage storage=%u result=0x%08x",
                     status->storageID, last);
            if (R_SUCCEEDED(last)) {
                last = ncmContentStorageGetPath(
                    &storage, path, sizeof(path), &content_id);
                log_line("ncm-content-path result=0x%08x path=%s", last,
                         R_SUCCEEDED(last) ? path : "(unavailable)");
                if (R_FAILED(last)) ncmContentStorageClose(&storage);
            }
            if (R_FAILED(last)) {
                reported = prefer_romfs_failure(reported, last);
                if (is_update) update_failed = true;
                continue;
            }

            FsStorage data_storage = {0};
            last = open_data_storage_by_path(
                &data_storage, path, (FsContentAttributes)content_attributes, 1);
            log_line("fs-open-storage-path type=%u attributes=%u stage=before-license result=0x%08x",
                     status->meta_type, content_attributes, last);
            if (result_is_missing_external_key(last)) {
                Result license_result = prepare_installed_license(
                    &storage, &content_id,
                    (FsContentAttributes)content_attributes);
                if (R_FAILED(license_result))
                    log_line("license preparation did not complete; trying FS open");
                last = open_data_storage_by_path(
                    &data_storage, path,
                    (FsContentAttributes)content_attributes, 1);
                log_line("fs-open-storage-path type=%u attributes=%u stage=after-license result=0x%08x",
                         status->meta_type, content_attributes, last);
            }
            ncmContentStorageClose(&storage);
            if (R_SUCCEEDED(last)) {
                last = romfsMountFromStorage(data_storage, 0, "kdrom");
                log_line("romfs-storage-path type=%u result=0x%08x",
                         status->meta_type, last);
                if (R_SUCCEEDED(last)) {
                    if (!is_update && update_failed) {
                        g.romfs_base_only = true;
                        log_line("romfs-warning base-only: the update RomFS could not be opened; this RomFS is the base version without the update");
                    }
                    *romfs_device = true;
                    ncmExit();
                    return 0;
                }
            }
            reported = prefer_romfs_failure(reported, last);

            FsFileSystem filesystem = {0};
            last = fsOpenFileSystemWithId(
                &filesystem, application_id, (FsFileSystemType)1,
                path, (FsContentAttributes)content_attributes);
            log_line("fs-open-content type=%u result=0x%08x",
                     status->meta_type, last);
            if (R_SUCCEEDED(last)) {
                int device = fsdevMountDevice("kdrom", filesystem);
                log_line("fsdev-mount-content device=%d", device);
                if (device >= 0) {
                    if (!is_update && update_failed) {
                        g.romfs_base_only = true;
                        log_line("romfs-warning base-only: the update RomFS could not be opened; this RomFS is the base version without the update");
                    }
                    ncmExit();
                    return 0;
                }
                last = MAKERESULT(
                    Module_Libnx, LibnxError_TooManyDevOpTabs);
                log_result("fsdev-mount-content", last);
            }
            reported = prefer_romfs_failure(reported, last);
            if (is_update) update_failed = true;
        }
    }
    ncmExit();
    if (R_FAILED(reported)) {
        log_line("romfs-explicit-failure reported=0x%08x canonical=0x%06x name=%s last=0x%08x",
                 reported, R_VALUE(reported), result_name(reported), last);
        return reported;
    }
    return last;
}

static Result mount_native_title_romfs(u64 application_id,
                                       bool *romfs_device);
static Result mount_self_title_romfs(u64 application_id,
                                     bool *romfs_device);

Result mount_title_romfs(u64 application_id, bool *romfs_device) {
    Result self_result = mount_self_title_romfs(application_id, romfs_device);
    log_line("romfs-self-title title=%016llX result=0x%08x",
             (unsigned long long)application_id, self_result);
    if (R_SUCCEEDED(self_result)) return 0;

    Result native_result = mount_native_title_romfs(
        application_id, romfs_device);
    log_line("romfs-native-title title=%016llX result=0x%08x",
             (unsigned long long)application_id, native_result);
    if (R_SUCCEEDED(native_result)) return 0;

    /* The remaining routes all open the application's first Program. */
    if (g_program_index_override > 0)
        return prefer_romfs_failure(native_result, self_result);

    *romfs_device = true;
    Result result = romfsMountDataStorageFromProgram(application_id, "kdrom");
    log_line("romfs-program title=%016llX result=0x%08x",
             (unsigned long long)application_id, result);
    if (R_SUCCEEDED(result)) return result;

    FsFileSystem filesystem = {0};
    result = fsOpenFileSystemWithPatch(
        &filesystem, application_id, (FsFileSystemType)1);
    log_line("fs-open-patch title=%016llX result=0x%08x",
             (unsigned long long)application_id, result);
    if (R_SUCCEEDED(result)) {
        *romfs_device = false;
        int device = fsdevMountDevice("kdrom", filesystem);
        log_line("fsdev-mount-patch device=%d", device);
        if (device >= 0) return 0;
        result = MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
        log_result("fsdev-mount-patch", result);
    }

    *romfs_device = false;
    result = mount_explicit_program_content(application_id, romfs_device);
    if (R_SUCCEEDED(result)) return result;
    return prefer_romfs_failure(result, self_result);
}

Result mount_base_title_romfs(u64 application_id, bool *romfs_device) {
    /* The explicit route only reaches the application's first Program. */
    if (g_program_index_override > 0) {
        log_line("romfs-base-unavailable index=%d reason=explicit-route-first-program-only",
                 g_program_index_override);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    *romfs_device = false;
    Result result = mount_explicit_program_content(application_id,
                                                   romfs_device);
    log_line("romfs-base-title title=%016llX result=0x%08x",
             (unsigned long long)application_id, result);
    return result;
}

/* ExeFS ------------------------------------------------------------------ */

static Result open_code_filesystem(FsFileSystem *out, u64 program_id,
                                   u8 storage_id, u8 attributes) {
    const u64 started = armGetSystemTick();
    FsCodeInfo code_info = {0};
    Handle loader_override = smGetServiceOverride(smEncodeName("fsp-ldr"));
    log_line("exefs-fsldr-begin program=%016llX storage=%u attributes=%u override=%s handle=0x%x",
             (unsigned long long)program_id, storage_id, attributes,
             loader_override != INVALID_HANDLE ? "present" : "absent",
             loader_override);

    if (loader_override == INVALID_HANDLE) {
        Result result = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        log_result("exefs-fsldr-missing-override", result);
        log_line("exefs-fsldr-finish program=%016llX storage=%u attributes=%u signed=0 result=0x%08x elapsed-us=%llu",
                 (unsigned long long)program_id, storage_id, attributes,
                 result,
                 (unsigned long long)(armTicksToNs(
                     armGetSystemTick() - started) / 1000));
        return result;
    }

    Result result = fsldrInitialize();
    log_result("exefs-fsldr-initialize", result);
    if (R_SUCCEEDED(result)) {
        result = fsldrOpenCodeFileSystem(
            &code_info, program_id, (NcmStorageId)storage_id, "",
            (FsContentAttributes)attributes, out);
        log_result("exefs-fsldr-open-code", result);
        if (R_FAILED(result)) fsldrExit();
    }
    log_line("exefs-fsldr-finish program=%016llX storage=%u attributes=%u signed=%u session=%s result=0x%08x elapsed-us=%llu",
             (unsigned long long)program_id, storage_id, attributes,
             code_info.is_signed, R_SUCCEEDED(result) ? "retained" : "released",
             result,
             (unsigned long long)(armTicksToNs(
                 armGetSystemTick() - started) / 1000));
    return result;
}

void close_code_filesystem(FsFileSystem *filesystem) {
    fsFsClose(filesystem);
    fsldrExit();
    log_line("exefs-fsldr-session-released");
}

static void log_exefs_root(FsFileSystem *filesystem, u64 attempt) {
    FsDir directory = {0};
    Result result = fsFsOpenDirectory(
        filesystem, "/", FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles,
        &directory);
    log_line("exefs-diagnostic-root-open attempt=%llu result=0x%08x",
             (unsigned long long)attempt, result);
    if (R_FAILED(result)) return;

    u32 entries = 0;
    for (; entries < 64; ++entries) {
        FsDirectoryEntry entry = {0};
        s64 count = 0;
        result = fsDirRead(&directory, &count, 1, &entry);
        if (R_FAILED(result) || count == 0) break;
        log_line("exefs-diagnostic-root-entry attempt=%llu index=%u type=%u size=%lld name=%s",
                 (unsigned long long)attempt, entries, entry.type,
                 (long long)entry.file_size, entry.name);
    }
    fsDirClose(&directory);
    log_line("exefs-diagnostic-root-finish attempt=%llu result=0x%08x entries=%u truncated=%u",
             (unsigned long long)attempt, result, entries, entries == 64);
}

static Result resolve_installed_content_impl(
    const NsApplicationContentMetaStatus *status, u8 content_type,
    char *path, size_t path_size, FsContentAttributes *attributes,
    bool prepare_license, u64 *runtime_program_id,
    u8 *runtime_program_index) {
    Result result = ncmInitialize();
    if (R_FAILED(result)) return result;

    NcmContentMetaDatabase database = {0};
    NcmContentStorage storage = {0};
    NcmContentId content_id = {0};
    NcmContentMetaKey key = {
        .id = status->application_id,
        .version = status->version,
        .type = status->meta_type,
        .install_type = NcmContentInstallType_Full,
    };
    u8 content_attributes = 0;
    u8 content_platform = 0xff;
    u8 content_id_offset = 0;

    result = ncmOpenContentMetaDatabase(&database, status->storageID);
    if (R_SUCCEEDED(result)) {
        result = select_installed_content(
            &database, &key, content_type, &content_id,
            &content_attributes, &content_platform, &content_id_offset);
        ncmContentMetaDatabaseClose(&database);
    }
    if (R_SUCCEEDED(result))
        result = ncmOpenContentStorage(&storage, status->storageID);
    if (R_SUCCEEDED(result)) {
        result = ncmContentStorageGetPath(
            &storage, path, path_size, &content_id);
        if (R_SUCCEEDED(result) && runtime_program_id) {
            Result program_result = ncmContentStorageGetProgramId(
                &storage, runtime_program_id, &content_id,
                (FsContentAttributes)content_attributes);
            log_line("selected-content-program-id content=%016llX runtime=%016llX result=0x%08x",
                     (unsigned long long)status->application_id,
                     (unsigned long long)(R_SUCCEEDED(program_result)
                         ? *runtime_program_id : 0),
                     program_result);
            if (R_FAILED(program_result)) *runtime_program_id = 0;
        }
        if (R_SUCCEEDED(result) && prepare_license) {
            Result license_result = prepare_installed_license(
                &storage, &content_id,
                (FsContentAttributes)content_attributes);
            if (R_FAILED(license_result))
                log_line("selected-content license preparation did not complete");
        }
        ncmContentStorageClose(&storage);
    }
    ncmExit();

    *attributes = (FsContentAttributes)content_attributes;
    if (runtime_program_index)
        *runtime_program_index = content_id_offset;
    log_line("selected-content-resolve id=%016llX meta-type=%u content-type=%u storage=%u version=%u platform=%u id-offset=%u attributes=%u prepare-license=%u result=0x%08x path=%s",
             (unsigned long long)status->application_id, status->meta_type,
             content_type, status->storageID, status->version,
             content_platform, content_id_offset, content_attributes,
             prepare_license, result,
             R_SUCCEEDED(result) ? path : "(unavailable)");
    return result;
}

Result resolve_installed_content(
    const NsApplicationContentMetaStatus *status, u8 content_type,
    char *path, size_t path_size, FsContentAttributes *attributes) {
    return resolve_installed_content_impl(
        status, content_type, path, path_size, attributes, true, NULL, NULL);
}

static Result resolve_installed_content_info(
    const NsApplicationContentMetaStatus *status, u8 content_type,
    char *path, size_t path_size, FsContentAttributes *attributes,
    u64 *runtime_program_id, u8 *runtime_program_index) {
    return resolve_installed_content_impl(
        status, content_type, path, path_size, attributes, false,
        runtime_program_id, runtime_program_index);
}

/* The installed status of this type with the highest version. NS can list
   an update that is no longer installed (no storage), and the console runs
   the newest installed one, so the choice rows (build_content_choices) and
   every export route use this same entry. */
const NsApplicationContentMetaStatus *find_program_status(u8 meta_type) {
    const NsApplicationContentMetaStatus *found = NULL;
    for (s32 i = 0; i < g.meta_count; ++i) {
        if (g.meta[i].meta_type != meta_type ||
            g.meta[i].storageID == NcmStorageId_None)
            continue;
        if (!found || g.meta[i].version > found->version)
            found = &g.meta[i];
    }
    return found;
}

/* DLC updates ------------------------------------------------------------ */

/* The installed DLC update (DataPatch) of a DLC, newest version. Its id is
   the DLC's + 0x800 (e.g. 010015100B515001 -> 010015100B515801). */
const NsApplicationContentMetaStatus *find_dlc_update(
    const NsApplicationContentMetaStatus *dlc) {
    if (!dlc || dlc->meta_type != NcmContentMetaType_AddOnContent)
        return NULL;
    const NsApplicationContentMetaStatus *found = NULL;
    for (s32 i = 0; i < g.meta_count; ++i) {
        const NsApplicationContentMetaStatus *status = &g.meta[i];
        if (status->meta_type != NcmContentMetaType_DataPatch ||
            status->storageID == NcmStorageId_None ||
            status->application_id != dlc->application_id + 0x800)
            continue;
        if (!found || status->version > found->version) found = status;
    }
    return found;
}

/* Read-only diagnostic: the DLC paths registered in the add-on content
   location resolver (lr 3). Since 15.0.0 it can hold a second path per DLC
   (GetRegisteredAddOnContentPaths, 5), which is where a DLC update is
   paired with its DLC. Nothing is registered or changed here. */
static void log_registered_addon_paths(u64 data_id) {
    Result result = lrInitialize();
    if (R_FAILED(result)) {
        log_result("dlc-update-lr-initialize", result);
        return;
    }
    Service resolver = {0};
    result = serviceDispatch(lrGetServiceSession(), 3,
        .out_num_objects = 1,
        .out_objects = &resolver,
    );
    if (R_SUCCEEDED(result)) {
        char path[0x300] = {0};
        char other[0x300] = {0};
        result = serviceDispatchIn(&resolver, 5, data_id,
            .buffer_attrs = {
                SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
                SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
            },
            .buffers = {{path, sizeof(path)}, {other, sizeof(other)}},
        );
        path[sizeof(path) - 1] = 0;
        other[sizeof(other) - 1] = 0;
        log_line("dlc-update-lr-registered dlc=%016llX result=0x%08x path=%s other=%s",
                 (unsigned long long)data_id, result,
                 R_SUCCEEDED(result) && path[0] ? path : "(none)",
                 R_SUCCEEDED(result) && other[0] ? other : "(none)");
        serviceClose(&resolver);
    } else {
        log_result("dlc-update-lr-open-addon", result);
    }
    lrExit();
}

static s64 storage_size(FsStorage *storage) {
    s64 size = -1;
    if (R_FAILED(fsStorageGetSize(storage, &size))) size = -1;
    return size;
}

Result open_patched_addon_storage(const NsApplicationContentMetaStatus *dlc,
                                  const NsApplicationContentMetaStatus *patch,
                                  FsStorage *out) {
    char patch_path[FS_MAX_PATH] = {0};
    FsContentAttributes patch_attributes = 0;
    /* Also prepares the update's license when it needs one. */
    Result result = resolve_installed_content(
        patch, NcmContentType_Data, patch_path, sizeof(patch_path),
        &patch_attributes);
    log_line("dlc-update-resolve dlc=%016llX update=%016llX version=%u storage=%u result=0x%08x path=%s",
             (unsigned long long)dlc->application_id,
             (unsigned long long)patch->application_id, patch->version,
             patch->storageID, result,
             R_SUCCEEDED(result) ? patch_path : "(unavailable)");
    if (R_FAILED(result)) return result;
    log_registered_addon_paths(dlc->application_id);

    /* A DLC update is a delta on top of its DLC, like a game update. Opened
       on its own it reports the size of the updated DLC, but reads of data
       that comes from the DLC fail. A storage of exactly that size, opened
       by DLC id, is the merged DLC; the size of the plain DLC means FS
       ignored the update. */
    s64 base_size = -1;
    s64 patch_size = -1;
    FsStorage probe = {0};
    Result base_result = fsOpenDataStorageByDataId(
        &probe, dlc->application_id, (NcmStorageId)dlc->storageID);
    if (R_SUCCEEDED(base_result)) {
        base_size = storage_size(&probe);
        fsStorageClose(&probe);
    }
    Result patch_result = open_data_storage_by_path(
        &probe, patch_path, patch_attributes, FsFileSystemType_ContentData);
    if (R_SUCCEEDED(patch_result)) {
        patch_size = storage_size(&probe);
        fsStorageClose(&probe);
    }
    log_line("dlc-update-sizes dlc=%016llX base-result=0x%08x base=%lld update-result=0x%08x updated=%lld",
             (unsigned long long)dlc->application_id, base_result,
             (long long)base_size, patch_result, (long long)patch_size);
    if (patch_size < 0 || patch_size == base_size) {
        /* Without a distinct updated size a merged DLC cannot be told
           apart from the plain one, so do not claim one. */
        log_line("dlc-update-unverifiable dlc=%016llX",
                 (unsigned long long)dlc->application_id);
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    /* With no explicit storage FS resolves the DLC through the add-on
       content location resolver, where the update can be paired. */
    const u8 storages[] = {NcmStorageId_Any, NcmStorageId_None};
    for (size_t i = 0; i < sizeof(storages); ++i) {
        FsStorage candidate = {0};
        result = fsOpenDataStorageByDataId(
            &candidate, dlc->application_id, (NcmStorageId)storages[i]);
        const s64 size = R_SUCCEEDED(result) ? storage_size(&candidate) : -1;
        const bool merged = R_SUCCEEDED(result) && size == patch_size;
        log_line("dlc-update-open dlc=%016llX storage=%u result=0x%08x size=%lld merged=%u",
                 (unsigned long long)dlc->application_id, storages[i],
                 result, (long long)size, merged);
        if (merged) {
            *out = candidate;
            return 0;
        }
        if (R_SUCCEEDED(result)) fsStorageClose(&candidate);
    }
    return MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

static bool result_is_missing_external_key(Result result) {
    return (result & 0x3fffff) == 0x7d802;
}

s32 content_program_indices(const NsApplicationContentMetaStatus *status,
                            u8 *out, s32 capacity) {
    if (!status || capacity <= 0) return 0;
    Result result = ncmInitialize();
    if (R_FAILED(result)) return 0;

    s32 count = 0;
    NcmContentMetaDatabase database = {0};
    result = ncmOpenContentMetaDatabase(&database, status->storageID);
    if (R_SUCCEEDED(result)) {
        NcmContentMetaKey key = {
            .id = status->application_id,
            .version = status->version,
            .type = status->meta_type,
            .install_type = NcmContentInstallType_Full,
        };
        NcmContentInfo infos[32] = {0};
        s32 written = 0;
        result = ncmContentMetaDatabaseListContentInfo(
            &database, &written, infos, 32, &key, 0);
        for (s32 i = 0; R_SUCCEEDED(result) && i < written; ++i) {
            if (infos[i].content_type != NcmContentType_Program) continue;
            const u8 index = infos[i].id_offset;
            s32 at = 0;
            while (at < count && out[at] < index) ++at;
            if (at < count && out[at] == index) continue;
            if (count >= capacity) break;
            memmove(&out[at + 1], &out[at], (size_t)(count - at));
            out[at] = index;
            ++count;
        }
        ncmContentMetaDatabaseClose(&database);
    }
    ncmExit();
    log_line("program-indices id=%016llX type=%u result=0x%08x count=%d",
             (unsigned long long)status->application_id, status->meta_type,
             result, count);
    return R_SUCCEEDED(result) ? count : 0;
}

s32 game_program_indices(u8 *out, s32 capacity) {
    const NsApplicationContentMetaStatus *update =
        find_program_status(NcmContentMetaType_Patch);
    return content_program_indices(
        update ? update : find_program_status(NcmContentMetaType_Application),
        out, capacity);
}

void load_status_programs(const NsApplicationContentMetaStatus *status,
                          GameProgramList *out) {
    memset(out, 0, sizeof(*out));
    out->platform = PROGRAM_PLATFORM_UNKNOWN;
    if (!status || status->storageID == NcmStorageId_None) return;

    Result result = ncmInitialize();
    if (R_FAILED(result)) return;

    NcmContentMetaKey key = {
        .id = status->application_id,
        .version = status->version,
        .type = status->meta_type,
        .install_type = NcmContentInstallType_Full,
    };
    NcmContentId content_ids[MAX_GAME_PROGRAMS] = {0};
    u8 attrs[MAX_GAME_PROGRAMS] = {0};
    NcmContentMetaDatabase database = {0};
    result = ncmOpenContentMetaDatabase(&database, status->storageID);
    if (R_SUCCEEDED(result)) {
        u8 platform = PROGRAM_PLATFORM_UNKNOWN;
        if (R_SUCCEEDED(ncmContentMetaDatabaseGetPlatform(
                &database, &platform, &key)))
            out->platform = platform;

        NcmContentInfo infos[32] = {0};
        s32 written = 0;
        result = ncmContentMetaDatabaseListContentInfo(
            &database, &written, infos, 32, &key, 0);
        for (s32 i = 0; R_SUCCEEDED(result) && i < written; ++i) {
            if (infos[i].content_type != NcmContentType_Program) continue;
            const u8 index = infos[i].id_offset;
            int at = 0;
            while (at < out->count && out->indices[at] < index) ++at;
            if (at < out->count && out->indices[at] == index) continue;
            if (out->count >= MAX_GAME_PROGRAMS) break;
            for (int j = out->count; j > at; --j) {
                out->indices[j] = out->indices[j - 1];
                content_ids[j] = content_ids[j - 1];
                attrs[j] = attrs[j - 1];
            }
            out->indices[at] = index;
            content_ids[at] = infos[i].content_id;
            attrs[at] = infos[i].attr & 0x0f;
            ++out->count;
        }
        ncmContentMetaDatabaseClose(&database);
    }

    if (R_SUCCEEDED(result) && out->count > 1) {
        NcmContentStorage storage = {0};
        if (R_SUCCEEDED(ncmOpenContentStorage(&storage, status->storageID))) {
            for (int i = 0; i < out->count; ++i) {
                u64 runtime_id = 0;
                Result id_result = ncmContentStorageGetProgramId(
                    &storage, &runtime_id, &content_ids[i],
                    (FsContentAttributes)attrs[i]);
                if (R_SUCCEEDED(id_result)) out->runtime_ids[i] = runtime_id;
                log_line("program-info id=%016llX type=%u index=%u runtime=%016llX result=0x%08x",
                         (unsigned long long)status->application_id,
                         status->meta_type, out->indices[i],
                         (unsigned long long)out->runtime_ids[i], id_result);
            }
            ncmContentStorageClose(&storage);
        }
    } else if (R_FAILED(result)) {
        out->count = 0;
    }
    ncmExit();
}

typedef struct {
    LrRegisteredLocationResolver resolver;
    u64 program_id;
    u64 owner_id;
    bool lr_initialized;
    bool resolver_open;
    bool registered;
    bool had_previous;
    char previous_path[FS_MAX_PATH];
    u8 previous_attributes[16];
} TemporaryProgramLocation;

typedef struct {
    u64 program_id;
    u64 base_program_id;
    u8 program_index;
    u8 flags;
    u8 reserved[14];
} ProgramIndexMapEntry;

typedef struct {
    ProgramIndexMapEntry previous[32];
    s32 previous_count;
    bool registered;
} TemporaryProgramIndexMap;

static Result set_program_index_map(
    const ProgramIndexMapEntry *entries, s32 count) {
    Service *fsp = fsGetServiceSession();
    serviceAssumeDomain(fsp);
    return serviceDispatchIn(fsp, 810, count,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{entries, (size_t)count * sizeof(*entries)}},
    );
}

static void add_program_index_map_entry(
    ProgramIndexMapEntry *entries, s32 *count, s32 capacity,
    u64 program_id, u64 base_program_id, u8 program_index) {
    for (s32 i = 0; i < *count; ++i) {
        if (entries[i].program_id != program_id) continue;
        entries[i].base_program_id = base_program_id;
        entries[i].program_index = program_index;
        return;
    }
    if (*count >= capacity) return;
    entries[*count].program_id = program_id;
    entries[*count].base_program_id = base_program_id;
    entries[*count].program_index = program_index;
    ++*count;
}

static Result begin_temporary_program_index_map(
    TemporaryProgramIndexMap *map, u64 application_id,
    u64 runtime_program_id, u8 program_index) {
    memset(map, 0, sizeof(*map));

    u32 current_index = 0;
    u32 current_count = 0;
    Result result = fsGetProgramIndexForAccessLog(
        &current_index, &current_count);
    log_line("exefs-index-map-current result=0x%08x index=%u count=%u",
             result, current_index, current_count);
    if (R_FAILED(result)) {
        if ((result & 0x3fffff) != 0x320002) return result;
        current_index = 0;
        current_count = 1;
        log_line("exefs-index-map-current-unavailable assuming-current-homebrew index=0 count=1");
    }
    if (current_count > 1 || (current_count && current_index != 0)) {
        result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        log_result("exefs-index-map-current-invalid", result);
        return result;
    }

    u64 own_program_id = 0;
    result = svcGetInfo(
        &own_program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
    log_line("exefs-index-map-own-program result=0x%08x id=%016llX",
             result, (unsigned long long)own_program_id);
    if (R_FAILED(result)) return result;

    /* FS does not expose the complete map, so only replace a map that is
     * empty or contains this single, index-zero homebrew process. */
    u64 current_base_id = own_program_id;
    map->previous_count = (s32)current_count;
    for (u32 i = 0; i < current_count; ++i) {
        map->previous[i].program_id = current_base_id + i;
        map->previous[i].base_program_id = current_base_id;
        map->previous[i].program_index = (u8)i;
    }

    ProgramIndexMapEntry active[34] = {0};
    s32 active_count = map->previous_count;
    memcpy(active, map->previous,
           (size_t)map->previous_count * sizeof(*active));
    add_program_index_map_entry(
        active, &active_count, 34, application_id, application_id, 0);
    add_program_index_map_entry(
        active, &active_count, 34, runtime_program_id, application_id,
        program_index);

    result = set_program_index_map(active, active_count);
    log_line("exefs-index-map-register application=%016llX runtime=%016llX index=%u previous=%d active=%d result=0x%08x",
             (unsigned long long)application_id,
             (unsigned long long)runtime_program_id, program_index,
             map->previous_count, active_count, result);
    if (R_SUCCEEDED(result)) map->registered = true;
    return result;
}

static Result end_temporary_program_index_map(
    TemporaryProgramIndexMap *map) {
    if (!map->registered) return 0;
    Result result = set_program_index_map(
        map->previous, map->previous_count);
    log_line("exefs-index-map-restore count=%d result=0x%08x",
             map->previous_count, result);
    memset(map, 0, sizeof(*map));
    return result;
}

static TemporaryProgramLocation g_romfs_program_location;
static TemporaryProgramIndexMap g_romfs_program_index_map;
static bool g_romfs_native_context_active;

static Result begin_temporary_program_location(
    TemporaryProgramLocation *location, u64 program_id, u64 owner_id,
    const char *path, u8 attributes);
static Result end_temporary_program_location(
    TemporaryProgramLocation *location);

static void end_native_romfs_context(void) {
    if (!g_romfs_native_context_active) return;
    Result map_result = end_temporary_program_index_map(
        &g_romfs_program_index_map);
    if (R_FAILED(map_result))
        log_result("romfs-index-map-cleanup", map_result);
    Result location_result = end_temporary_program_location(
        &g_romfs_program_location);
    if (R_FAILED(location_result))
        log_result("romfs-location-cleanup", location_result);
    g_romfs_native_context_active = false;
}

static Result mount_native_status_romfs(
    u64 application_id, const NsApplicationContentMetaStatus *selected,
    const NsApplicationContentMetaStatus *base, bool allow_patch,
    bool *romfs_device) {
    end_native_romfs_context();

    if (!selected)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    char path[FS_MAX_PATH] = {0};
    FsContentAttributes attributes = 0;
    u64 runtime_program_id = 0;
    u8 program_index = 0;
    Result result = resolve_installed_content_info(
        selected, NcmContentType_Program, path, sizeof(path), &attributes,
        &runtime_program_id, &program_index);
    log_line("romfs-native-resolve application=%016llX runtime=%016llX index=%u attributes=%u result=0x%08x path=%s",
             (unsigned long long)application_id,
             (unsigned long long)runtime_program_id, program_index,
             attributes, result, R_SUCCEEDED(result) ? path : "(unavailable)");
    if (R_FAILED(result)) return result;

    /* Ordinary titles already have a working system location. This route is
     * for Programs with their own runtime ID: a Switch 2 Edition's native
     * Program (index >= 1), or a Switch 1 game whose update swaps in a
     * native Program at index 0. */
    if (runtime_program_id == application_id)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);

    result = begin_temporary_program_location(
        &g_romfs_program_location, runtime_program_id, application_id,
        path, (u8)attributes);
    if (R_FAILED(result)) return result;
    g_romfs_native_context_active = true;

    result = begin_temporary_program_index_map(
        &g_romfs_program_index_map, application_id, runtime_program_id,
        program_index);
    if (R_FAILED(result)) {
        end_native_romfs_context();
        return result;
    }

    FsFileSystem filesystem = {0};
    if (allow_patch) {
        result = fsOpenFileSystemWithPatch(
            &filesystem, runtime_program_id, (FsFileSystemType)1);
        log_line("romfs-native-open-patch runtime=%016llX result=0x%08x",
                 (unsigned long long)runtime_program_id, result);
        if (R_FAILED(result) && base) {
            /* The fallbacks below open the selected Program on its own. When
               the base has this Program too, the update's is only the delta
               on top of it: it can mount, but every file that comes from the
               base fails to read. A Program the update adds (a Switch 2
               Edition's native program) is complete, so it may still be
               opened alone. */
            u8 base_programs[MAX_GAME_PROGRAMS] = {0};
            const s32 base_count = content_program_indices(
                base, base_programs, MAX_GAME_PROGRAMS);
            bool base_has_program = false;
            for (s32 i = 0; i < base_count; ++i)
                if (base_programs[i] == program_index) base_has_program = true;
            log_line("romfs-native-update-program index=%u base-has-program=%u",
                     program_index, base_has_program);
            if (base_has_program) {
                log_line("romfs-native-skip-update-only reason=update-romfs-needs-base");
                end_native_romfs_context();
                return result;
            }
        }
    } else {
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    if (R_FAILED(result)) {
        result = fsOpenFileSystemWithId(
            &filesystem, runtime_program_id, (FsFileSystemType)1,
            path, attributes);
        log_line("romfs-native-open-id runtime=%016llX result=0x%08x",
                 (unsigned long long)runtime_program_id, result);
    }
    if (R_SUCCEEDED(result)) {
        int device = fsdevMountDevice("kdrom", filesystem);
        log_line("romfs-native-mount-filesystem device=%d", device);
        if (device >= 0) {
            *romfs_device = false;
            return 0;
        }
        fsFsClose(&filesystem);
        result = MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
    }

    FsStorage storage = {0};
    Result storage_result = open_data_storage_by_path(
        &storage, path, attributes, 1);
    log_line("romfs-native-open-storage runtime=%016llX result=0x%08x",
             (unsigned long long)runtime_program_id, storage_result);
    if (R_SUCCEEDED(storage_result)) {
        storage_result = romfsMountFromStorage(storage, 0, "kdrom");
        log_line("romfs-native-mount-storage result=0x%08x",
                 storage_result);
        if (R_SUCCEEDED(storage_result)) {
            *romfs_device = true;
            return 0;
        }
        fsStorageClose(&storage);
    }

    end_native_romfs_context();
    return R_FAILED(result) ? result : storage_result;
}

static Result mount_native_title_romfs(u64 application_id,
                                       bool *romfs_device) {
    const NsApplicationContentMetaStatus *base =
        find_program_status(NcmContentMetaType_Application);
    const NsApplicationContentMetaStatus *update =
        find_program_status(NcmContentMetaType_Patch);
    const NsApplicationContentMetaStatus *selected = update ? update : base;
    return mount_native_status_romfs(
        application_id, selected, selected == update ? base : NULL,
        true, romfs_device);
}

Result mount_base_program_romfs(u64 application_id,
                                const NsApplicationContentMetaStatus *status,
                                bool *romfs_device) {
    if (!find_program_status(NcmContentMetaType_Patch))
        return mount_title_romfs(application_id, romfs_device);
    return mount_native_status_romfs(
        application_id, status, NULL, false, romfs_device);
}

/* When KaiDump is launched through the selected game, this session lets FS
 * return the same patched RomFS that the running game sees. This is useful
 * for titles whose update key cannot be prepared through ES. */
static Service g_self_fs;
static bool g_self_fs_active;

static void close_self_filesystem_session(void) {
    if (!g_self_fs_active) return;
    serviceClose(&g_self_fs);
    g_self_fs_active = false;
    log_line("romfs-self-session-closed");
}

static Result open_self_filesystem_session(void) {
    if (g_self_fs_active) return 0;

    Handle session = INVALID_HANDLE;
    Result result = smGetServiceOriginal(&session, smEncodeName("fsp-srv"));
    log_line("romfs-self-session-open result=0x%08x", result);
    if (R_FAILED(result)) return result;

    serviceCreate(&g_self_fs, session);
    const u64 pid_placeholder = 0;
    result = serviceDispatchIn(&g_self_fs, 1, pid_placeholder,
        .in_send_pid = true,
    );
    log_result("romfs-self-set-current-process", result);
    if (R_FAILED(result)) {
        serviceClose(&g_self_fs);
        return result;
    }

    g_self_fs_active = true;
    return 0;
}

static Result open_self_data_storage(FsStorage *out, bool current_program,
                                     u8 program_index) {
    Result result = open_self_filesystem_session();
    if (R_FAILED(result)) return result;

    if (current_program) {
        result = serviceDispatch(&g_self_fs, 200,
            .out_num_objects = 1,
            .out_objects = &out->s,
        );
        log_result("romfs-self-open-current-process", result);
        if (R_SUCCEEDED(result)) return 0;
    }

    Result index_result = serviceDispatchIn(&g_self_fs, 205, program_index,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
    log_line("romfs-self-open-program-index index=%u result=0x%08x",
             program_index, index_result);
    return R_SUCCEEDED(index_result) || !current_program ? index_result
                                                         : result;
}

/* Runtime program id of the Program at a given index (or the automatic
   choice for -1). */
static u64 program_runtime_id(const NsApplicationContentMetaStatus *status,
                              int program_index, u8 *out_index) {
    const int previous = g_program_index_override;
    g_program_index_override = program_index;
    char path[FS_MAX_PATH] = {0};
    FsContentAttributes attributes = 0;
    u64 runtime_program_id = 0;
    u8 resolved_index = 0;
    Result result = resolve_installed_content_info(
        status, NcmContentType_Program, path, sizeof(path), &attributes,
        &runtime_program_id, &resolved_index);
    g_program_index_override = previous;
    if (out_index) *out_index = resolved_index;
    return R_SUCCEEDED(result) ? runtime_program_id : 0;
}

static Result mount_self_title_romfs(u64 application_id,
                                     bool *romfs_device) {
    close_self_filesystem_session();
    g.romfs_self_matched = false;

    u64 process_program_id = 0;
    Result id_result = svcGetInfo(&process_program_id, InfoType_ProgramId,
                                  CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(id_result)) process_program_id = 0;

    const NsApplicationContentMetaStatus *base =
        find_program_status(NcmContentMetaType_Application);
    const NsApplicationContentMetaStatus *update =
        find_program_status(NcmContentMetaType_Patch);
    const NsApplicationContentMetaStatus *selected = update ? update : base;

    /* The Program this export wants, and the one KaiDump is running as. */
    u8 wanted_index = 0;
    u64 wanted_runtime = selected
        ? program_runtime_id(selected, g_program_index_override,
                             &wanted_index)
        : 0;
    bool matches = false;
    u8 own_index = 0;
    if (process_program_id && process_program_id == wanted_runtime) {
        matches = true;
        own_index = wanted_index;
    } else if (process_program_id && selected) {
        u8 indices[MAX_GAME_PROGRAMS] = {0};
        s32 count = content_program_indices(selected, indices,
                                            MAX_GAME_PROGRAMS);
        for (s32 i = 0; i < count && !matches; ++i) {
            if (program_runtime_id(selected, indices[i], NULL) !=
                process_program_id)
                continue;
            matches = true;
            own_index = indices[i];
        }
    }

    log_line("romfs-self-check process=%016llX id-result=0x%08x application=%016llX runtime=%016llX wanted-index=%u own-index=%u match=%u",
             (unsigned long long)process_program_id, id_result,
             (unsigned long long)application_id,
             (unsigned long long)wanted_runtime, wanted_index, own_index,
             matches);
    if (!matches) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    g.romfs_self_matched = true;

    FsStorage storage = {0};
    Result result = open_self_data_storage(
        &storage, wanted_index == own_index, wanted_index);
    if (R_SUCCEEDED(result)) {
        s64 size = 0;
        Result size_result = fsStorageGetSize(&storage, &size);
        log_line("romfs-self-storage size-result=0x%08x size=%lld",
                 size_result, (long long)size);
        result = romfsMountFromStorage(storage, 0, "kdrom");
        log_line("romfs-self-mount result=0x%08x", result);
        if (R_SUCCEEDED(result)) {
            *romfs_device = true;
            return 0;
        }
    }

    close_self_filesystem_session();
    return result;
}

void unmount_title_romfs(const char *name, bool romfs_device) {
    if (romfs_device) romfsUnmount(name);
    else fsdevUnmountDevice(name);
    end_native_romfs_context();
    close_self_filesystem_session();
}

static bool result_is_missing_program_location(Result result) {
    u32 value = result & 0x3fffff;
    return value == 0x408 || value == 0x1408;
}

static Result write_registered_program_location(
    LrRegisteredLocationResolver *resolver, u64 program_id, u64 owner_id,
    const char *path, const u8 attributes[16]) {
    struct {
        u64 program_id;
        u64 owner_id;
        u8 attributes[16];
    } input = {
        .program_id = program_id,
        .owner_id = owner_id,
    };
    memcpy(input.attributes, attributes, sizeof(input.attributes));
    char send_path[FS_MAX_PATH] = {0};
    size_t path_length = strnlen(path, sizeof(send_path));
    if (path_length >= sizeof(send_path))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memcpy(send_path, path, path_length + 1);

    return serviceDispatchIn(&resolver->s, 1, input,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In},
        .buffers = {{send_path, sizeof(send_path)}},
    );
}

static Result end_temporary_program_location(
    TemporaryProgramLocation *location);

static Result begin_temporary_program_location(
    TemporaryProgramLocation *location, u64 program_id, u64 owner_id,
    const char *path, u8 attributes) {
    memset(location, 0, sizeof(*location));
    location->program_id = program_id;
    location->owner_id = owner_id;

    Result result = lrInitialize();
    log_result("exefs-location-initialize", result);
    if (R_FAILED(result)) return result;
    location->lr_initialized = true;

    result = lrOpenRegisteredLocationResolver(&location->resolver);
    log_result("exefs-location-open-registered", result);
    if (R_FAILED(result)) goto fail;
    location->resolver_open = true;

    Result previous_result = resolve_location_path(
        &location->resolver.s, 0, program_id, location->previous_path,
        location->previous_attributes);
    if (R_SUCCEEDED(previous_result)) {
        location->had_previous = true;
        log_line("exefs-location-previous program=%016llX attributes=%u path=%s",
                 (unsigned long long)program_id,
                 location->previous_attributes[0], location->previous_path);
    } else if (result_is_missing_program_location(previous_result)) {
        log_line("exefs-location-previous program=%016llX absent result=0x%08x",
                 (unsigned long long)program_id, previous_result);
    } else {
        result = previous_result;
        log_result("exefs-location-read-previous", result);
        goto fail;
    }

    u8 location_attributes[16] = {attributes};
    result = write_registered_program_location(
        &location->resolver, program_id, owner_id, path,
        location_attributes);
    log_line("exefs-location-register program=%016llX owner=%016llX attributes=%u result=0x%08x path=%s",
             (unsigned long long)program_id,
             (unsigned long long)owner_id, attributes, result, path);
    if (R_FAILED(result)) goto fail;
    location->registered = true;

    char verified_path[FS_MAX_PATH] = {0};
    u8 verified_attributes[16] = {0};
    result = resolve_location_path(
        &location->resolver.s, 0, program_id, verified_path,
        verified_attributes);
    log_line("exefs-location-verify program=%016llX attributes=%u result=0x%08x path=%s",
             (unsigned long long)program_id, verified_attributes[0], result,
             R_SUCCEEDED(result) ? verified_path : "(unavailable)");
    if (R_SUCCEEDED(result)) return 0;

fail:
    if (location->registered) {
        Result failure = result;
        end_temporary_program_location(location);
        return failure;
    }
    if (location->resolver_open) {
        serviceClose(&location->resolver.s);
        location->resolver_open = false;
    }
    if (location->lr_initialized) {
        lrExit();
        location->lr_initialized = false;
    }
    return result;
}

static Result end_temporary_program_location(
    TemporaryProgramLocation *location) {
    Result result = 0;
    if (location->registered && location->resolver_open) {
        if (location->had_previous) {
            result = write_registered_program_location(
                &location->resolver, location->program_id,
                location->owner_id, location->previous_path,
                location->previous_attributes);
            log_line("exefs-location-restore program=%016llX attributes=%u result=0x%08x path=%s",
                     (unsigned long long)location->program_id,
                     location->previous_attributes[0], result,
                     location->previous_path);
        } else {
            result = serviceDispatchIn(
                &location->resolver.s, 2, location->program_id);
            log_line("exefs-location-unregister program=%016llX result=0x%08x",
                     (unsigned long long)location->program_id, result);
        }
    }
    if (location->resolver_open)
        serviceClose(&location->resolver.s);
    if (location->lr_initialized)
        lrExit();
    memset(location, 0, sizeof(*location));
    return result;
}

/* Registered program locations ---------------------------------------------
 * fsp-ldr resolves a program through the RegisteredLocationResolver before
 * the storage's own resolver. NS registers the running game's program there
 * (pointing at its update), and an update's Program keeps the application's
 * program id, so without a registration of our own the wrong NCA is opened:
 *  - an update's ExeFS would come from the base game, and
 *  - while launched through the game, the base ExeFS would come from the
 *    update (with the base's attributes, failing 2002-4058).
 * Register the selected NCA for the duration of the open and restore the
 * previous registration afterwards. */
static bool registered_location_conflicts(u64 program_id, const char *path) {
    if (R_FAILED(lrInitialize())) return false;
    bool conflict = false;
    LrRegisteredLocationResolver resolver = {0};
    if (R_SUCCEEDED(lrOpenRegisteredLocationResolver(&resolver))) {
        char current[FS_MAX_PATH] = {0};
        u8 current_attributes[16] = {0};
        Result result = resolve_location_path(
            &resolver.s, 0, program_id, current, current_attributes);
        conflict = R_SUCCEEDED(result) && strcmp(current, path);
        log_line("exefs-registered-location program=%016llX result=0x%08x conflict=%u path=%s",
                 (unsigned long long)program_id, result, conflict,
                 R_SUCCEEDED(result) ? current : "(none)");
        serviceClose(&resolver.s);
    }
    lrExit();
    return conflict;
}

Result open_title_exefs(
    FsFileSystem *out, u64 program_id,
    const NsApplicationContentMetaStatus *selected,
    const NsApplicationContentMetaStatus *original) {
    if (!out || !selected)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const u64 attempt = ++g_exefs_attempt;
    const u64 started = armGetSystemTick();
    log_line("exefs-diagnostic-attempt-begin attempt=%llu application=%016llX selected=%016llX selected-type=%u original=%016llX original-type=%u",
             (unsigned long long)attempt, (unsigned long long)program_id,
             (unsigned long long)selected->application_id,
             selected->meta_type,
             (unsigned long long)(original ? original->application_id : 0),
             original ? original->meta_type : 0);
    log_program_content_diagnostics("selected", selected);
    log_program_content_diagnostics("original", original);

    char path[FS_MAX_PATH] = {0};
    FsContentAttributes attributes = 0;
    u64 runtime_program_id = 0;
    u8 runtime_program_index = 0;
    Result result = resolve_installed_content_info(
        selected, NcmContentType_Program,
        path, sizeof(path), &attributes, &runtime_program_id,
        &runtime_program_index);
    if (R_FAILED(result)) {
        log_line("exefs-diagnostic-attempt-finish attempt=%llu stage=resolve result=0x%08x elapsed-us=%llu",
                 (unsigned long long)attempt, result,
                 (unsigned long long)(armTicksToNs(
                     armGetSystemTick() - started) / 1000));
        return result;
    }
    if (!runtime_program_id) runtime_program_id = program_id;

    TemporaryProgramLocation location = {0};
    TemporaryProgramIndexMap index_map = {0};
    bool location_active = runtime_program_id != program_id;
    if (location_active) {
        result = begin_temporary_program_location(
            &location, runtime_program_id, program_id, path, (u8)attributes);
        if (R_FAILED(result)) {
            log_line("exefs-diagnostic-attempt-finish attempt=%llu stage=location-register result=0x%08x elapsed-us=%llu",
                     (unsigned long long)attempt, result,
                     (unsigned long long)(armTicksToNs(
                         armGetSystemTick() - started) / 1000));
            return result;
        }

        result = begin_temporary_program_index_map(
            &index_map, program_id, runtime_program_id,
            runtime_program_index);
        if (R_FAILED(result)) {
            Result cleanup_result = end_temporary_program_location(&location);
            if (R_FAILED(cleanup_result))
                log_result("exefs-location-cleanup", cleanup_result);
            log_line("exefs-diagnostic-attempt-finish attempt=%llu stage=index-map result=0x%08x elapsed-us=%llu",
                     (unsigned long long)attempt, result,
                     (unsigned long long)(armTicksToNs(
                         armGetSystemTick() - started) / 1000));
            return result;
        }
    }

    u8 storage_id = (u8)(original ? original->storageID : selected->storageID);

    TemporaryProgramLocation code_location = {0};
    bool code_location_active = false;
    if (!location_active &&
        ((original && selected->meta_type == NcmContentMetaType_Patch) ||
         registered_location_conflicts(runtime_program_id, path))) {
        result = begin_temporary_program_location(
            &code_location, runtime_program_id, program_id, path,
            (u8)attributes);
        if (R_FAILED(result)) {
            /* Opening without it would return another NCA's ExeFS. */
            log_line("exefs-diagnostic-attempt-finish attempt=%llu stage=code-location result=0x%08x elapsed-us=%llu",
                     (unsigned long long)attempt, result,
                     (unsigned long long)(armTicksToNs(
                         armGetSystemTick() - started) / 1000));
            return result;
        }
        code_location_active = true;
    }

    result = open_code_filesystem(
        out, runtime_program_id, storage_id, (u8)attributes);
    log_line("exefs-open-code application=%016llX runtime=%016llX selected=%016llX storage=%u attributes=%u result=0x%08x",
             (unsigned long long)program_id,
             (unsigned long long)runtime_program_id,
             (unsigned long long)selected->application_id,
             storage_id, attributes, result);

    const NsApplicationContentMetaStatus *licenses[] = {
        selected,
        original != selected ? original : NULL,
    };
    for (size_t i = 0;
         i < sizeof(licenses) / sizeof(licenses[0]) &&
             result_is_missing_external_key(result);
         ++i) {
        const NsApplicationContentMetaStatus *license = licenses[i];
        if (!license) continue;

        char license_path[FS_MAX_PATH] = {0};
        FsContentAttributes license_attributes = 0;
        Result prepare_result = resolve_installed_content(
            license, NcmContentType_Program,
            license_path, sizeof(license_path), &license_attributes);
        log_line("exefs-license-retry attempt=%llu source=%016llX type=%u attributes=%u path=%s result=0x%08x",
                 (unsigned long long)attempt,
                 (unsigned long long)license->application_id,
                 license->meta_type, license_attributes,
                 R_SUCCEEDED(prepare_result) ? license_path : "(unavailable)",
                 prepare_result);
        if (R_FAILED(prepare_result)) {
            result = prepare_result;
            break;
        }

        result = open_code_filesystem(
            out, runtime_program_id, storage_id, (u8)attributes);
        log_line("exefs-open-code-retry source=%016llX application=%016llX runtime=%016llX result=0x%08x",
                 (unsigned long long)license->application_id,
                 (unsigned long long)program_id,
                 (unsigned long long)runtime_program_id, result);
    }
    if (R_SUCCEEDED(result)) log_exefs_root(out, attempt);
    if (code_location_active) {
        Result code_cleanup = end_temporary_program_location(&code_location);
        if (R_FAILED(code_cleanup))
            log_result("exefs-code-location-cleanup", code_cleanup);
    }
    if (location_active) {
        Result map_cleanup_result = end_temporary_program_index_map(&index_map);
        if (R_FAILED(map_cleanup_result))
            log_result("exefs-index-map-cleanup", map_cleanup_result);
        Result cleanup_result = end_temporary_program_location(&location);
        if (R_FAILED(cleanup_result))
            log_result("exefs-location-cleanup", cleanup_result);
    }
    log_line("exefs-diagnostic-attempt-finish attempt=%llu stage=open result=0x%08x canonical=0x%06x name=%s elapsed-us=%llu",
             (unsigned long long)attempt, result, R_VALUE(result),
             result_name(result),
             (unsigned long long)(armTicksToNs(
                 armGetSystemTick() - started) / 1000));
    return result;
}
