#include "app_internal.h"

/* Archive preparation and SD, FTP, or USB export orchestration. */

/* Virtual archive sources ------------------------------------------------ */

typedef struct {
    bool exefs_active;
    bool exefs_fsldr_active;
    bool romfs_active;
    bool romfs_device;
    bool romfs_combined_name;
} NupMountContext;

static void unmount_nup_source(void *user) {
    NupMountContext *context = user;
    if (context->exefs_active) {
        fsdevUnmountDevice("kdnexe");
        context->exefs_active = false;
    }
    if (context->exefs_fsldr_active) {
        fsldrExit();
        context->exefs_fsldr_active = false;
        log_line("nup-exefs-fsldr-session-released");
    }
    if (context->romfs_active) {
        const char *name = context->romfs_combined_name ? "kdrom" : "kdnrom";
        unmount_title_romfs(name, context->romfs_device);
        context->romfs_active = false;
        context->romfs_combined_name = false;
    }
}

static void unmount_nup_exefs(NupMountContext *context) {
    if (context->exefs_active) {
        fsdevUnmountDevice("kdnexe");
        context->exefs_active = false;
    }
    if (context->exefs_fsldr_active) {
        fsldrExit();
        context->exefs_fsldr_active = false;
        log_line("nup-exefs-fsldr-session-released");
    }
}

static Result mount_nup_exefs_status(
    const NsApplicationContentMetaStatus *status,
    const NsApplicationContentMetaStatus *original_status,
    u64 program_id, NupMountContext *context) {
    FsFileSystem filesystem = {0};
    Result result = open_title_exefs(
        &filesystem, program_id, status, original_status);
    if (R_SUCCEEDED(result)) {
        int device = fsdevMountDevice("kdnexe", filesystem);
        log_line("nup-exefs-mount device=%d", device);
        if (device >= 0) {
            context->exefs_active = true;
            context->exefs_fsldr_active = true;
            return 0;
        }
        fsldrExit();
        log_line("nup-exefs-fsldr-session-released-after-mount-failure");
        result = MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
    }
    return result;
}

static Result mount_nup_combined_exefs(
    const ContentChoice *choice, NupMountContext *context,
    bool *exefs_base_only) {
    *exefs_base_only = false;
    Application *app = &g.applications[g.selected];
    s32 index = choice->update_index >= 0
        ? choice->update_index : choice->meta_index;
    const NsApplicationContentMetaStatus *original =
        index != choice->meta_index ? &g.meta[choice->meta_index] : NULL;
    Result result = mount_nup_exefs_status(
        &g.meta[index], original, app->record.application_id, context);
    if (R_FAILED(result) && original &&
        !(R_MODULE(result) == Module_Libnx &&
          R_DESCRIPTION(result) == LibnxError_TooManyDevOpTabs)) {
        log_line("exefs-warning base-only: the update ExeFS could not be opened (0x%08x); using the base ExeFS",
                 result);
        result = mount_nup_exefs_status(
            original, NULL, app->record.application_id, context);
        if (R_SUCCEEDED(result)) {
            g.exefs_base_only = true;
            *exefs_base_only = true;
        }
    }
    return result;
}

static Result mount_nup_romfs_status(
    const NsApplicationContentMetaStatus *status, NupMountContext *context,
    const NsApplicationContentMetaStatus **merged_update) {
    *merged_update = NULL;
    bool addon = status->meta_type == NcmContentMetaType_AddOnContent ||
                 status->meta_type == NcmContentMetaType_DataPatch;
    u8 content_type = addon ? NcmContentType_Data : NcmContentType_Program;
    u32 proxy_type = addon ? FsFileSystemType_ContentData : 1;
    char path[FS_MAX_PATH] = {0};
    FsContentAttributes attributes = 0;
    Result result = resolve_installed_content(
        status, content_type, path, sizeof(path), &attributes);
    if (R_FAILED(result)) return result;

    FsStorage storage = {0};
    const NsApplicationContentMetaStatus *update = find_dlc_update(status);
    if (update) {
        result = open_patched_addon_storage(status, update, &storage);
        if (R_SUCCEEDED(result)) {
            Result mount_result = romfsMountFromStorage(storage, 0, "kdnrom");
            log_line("nup-romfs-mount-dlc-update id=%016llX version=%u result=0x%08x",
                     (unsigned long long)status->application_id,
                     update->version, mount_result);
            if (R_SUCCEEDED(mount_result)) {
                context->romfs_active = true;
                context->romfs_device = true;
                *merged_update = update;
                return 0;
            }
            fsStorageClose(&storage);
            storage = (FsStorage){0};
        }
        log_line("dlc-update-warning not-included: the DLC update could not be merged; exporting the DLC without it");
        g.dlc_update_missing = true;
    }
    if (addon) {
        result = fsOpenDataStorageByDataId(
            &storage, status->application_id,
            (NcmStorageId)status->storageID);
        log_line("nup-romfs-open-data-id id=%016llX result=0x%08x",
                 (unsigned long long)status->application_id, result);
    } else {
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    if (R_FAILED(result))
        result = open_data_storage_by_path(
            &storage, path, attributes, proxy_type);
    log_line("nup-romfs-open-storage id=%016llX type=%u result=0x%08x",
             (unsigned long long)status->application_id,
             status->meta_type, result);
    if (R_SUCCEEDED(result)) {
        Result mount_result = romfsMountFromStorage(storage, 0, "kdnrom");
        log_line("nup-romfs-mount-storage id=%016llX result=0x%08x",
                 (unsigned long long)status->application_id, mount_result);
        if (R_SUCCEEDED(mount_result)) {
            context->romfs_active = true;
            context->romfs_device = true;
            return 0;
        }
        fsStorageClose(&storage);
        result = mount_result;
    }

    const Result storage_failure = result;
    FsFileSystem filesystem = {0};
    result = fsOpenFileSystemWithId(
        &filesystem, status->application_id,
        (FsFileSystemType)proxy_type, path, attributes);
    log_line("nup-romfs-open-filesystem id=%016llX type=%u result=0x%08x",
             (unsigned long long)status->application_id,
             status->meta_type, result);
    if (R_SUCCEEDED(result)) {
        int device = fsdevMountDevice("kdnrom", filesystem);
        log_line("nup-romfs-mount-filesystem device=%d", device);
        if (device >= 0) {
            context->romfs_active = true;
            context->romfs_device = false;
            return 0;
        }
        result = MAKERESULT(Module_Libnx, LibnxError_TooManyDevOpTabs);
    }
    return prefer_romfs_failure(storage_failure, result);
}

/* Application metadata, built once per export: NUP meta TLVs, and the
   control.nacp / icon files of extracted and TAR output. The "full" variant
   (with NACP and display version) describes the game as exported; a base
   exported without its installed update gets the other, so a 1.0.0 dump
   never carries the update's version. */
static uint8_t *g_nup_meta[2];
static size_t g_nup_meta_size[2];
static bool g_nup_meta_built[2];

static void free_nup_meta(void) {
    for (int i = 0; i < 2; ++i) {
        free(g_nup_meta[i]);
        g_nup_meta[i] = NULL;
        g_nup_meta_size[i] = 0;
        g_nup_meta_built[i] = false;
    }
}

static bool select_app_meta(const ContentChoice *choice, bool base_only,
                            const uint8_t **meta, size_t *meta_size) {
    *meta = NULL;
    *meta_size = 0;
    if (choice->kind != CONTENT_GAME && choice->kind != CONTENT_BASE)
        return false;
    const int full = !find_program_status(NcmContentMetaType_Patch) ||
                     (choice->kind == CONTENT_GAME && !base_only);
    if (!g_nup_meta_built[full]) {
        g_nup_meta_built[full] = true;
        build_nup_app_meta(full, &g_nup_meta[full], &g_nup_meta_size[full]);
    }
    *meta = g_nup_meta[full];
    *meta_size = g_nup_meta_size[full];
    return *meta != NULL;
}

static void attach_nup_meta(const ContentChoice *choice, bool base_only,
                            NupMount *mount) {
    /* A NUP packs one game/base package (whichever program was selected), so
       it always carries the application metadata TLVs; extracted/TAR output
       also gets control files per program. */
    const uint8_t *meta = NULL;
    size_t meta_size = 0;
    if (select_app_meta(choice, base_only, &meta, &meta_size)) {
        mount->meta = meta;
        mount->meta_size = meta_size;
    }
}

static Result write_memory_file(const char *path, const uint8_t *data,
                                uint32_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return MAKERESULT(Module_Libnx, LibnxError_IoError);
    bool ok = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        g.copied_bytes += size;
        ++g.copied_files;
    }
    return ok ? 0 : MAKERESULT(Module_Libnx, LibnxError_IoError);
}

/* Extracted to SD: the same control.nacp and icon files that the tree adds
   for FTP, USB and TAR (see tree_add_control_files in nup.c). */
static void write_control_files(const char *package, const ContentChoice *choice,
                                bool base_only) {
    const uint8_t *meta = NULL;
    size_t meta_size = 0;
    if (!select_app_meta(choice, base_only, &meta, &meta_size)) return;
    NupControlFile files[NUP_MAX_CONTROL_FILES];
    size_t count = nupControlFiles(meta, meta_size, files,
                                   NUP_MAX_CONTROL_FILES);
    size_t failed = 0;
    for (size_t i = 0; i < count; ++i) {
        char path[640];
        const int length = snprintf(path, sizeof(path), "%s/%s", package,
                                    files[i].name);
        if (length < 0 || (size_t)length >= sizeof(path) ||
            R_FAILED(write_memory_file(path, files[i].data, files[i].size)))
            ++failed;
    }
    log_line("control-files package=%s files=%zu failed=%zu", package, count,
             failed);
}

static void write_selected_control_files(const char *root,
                                         const ContentChoice *choice,
                                         bool base_only) {
    char package[512];
    if (choice->kind == CONTENT_BASE) {
        snprintf(package, sizeof(package), "%s/base", root);
        write_control_files(package, choice, base_only);
        u8 programs[MAX_GAME_PROGRAMS] = {0};
        s32 program_count = content_program_indices(
            &g.meta[choice->meta_index], programs, MAX_GAME_PROGRAMS);
        for (s32 i = 0; i < program_count; ++i) {
            if (!programs[i]) continue;
            snprintf(package, sizeof(package), "%s/base/program%u", root,
                     programs[i]);
            write_control_files(package, choice, base_only);
        }
        return;
    }
    if (choice->kind != CONTENT_GAME) return;
    snprintf(package, sizeof(package), "%s/game", root);
    write_control_files(package, choice, base_only);
    /* Switch 2 Editions: every program shares the application's control. */
    u8 programs[MAX_GAME_PROGRAMS] = {0};
    s32 program_count = game_program_indices(programs, MAX_GAME_PROGRAMS);
    for (s32 i = 0; i < program_count; ++i) {
        if (!programs[i]) continue;
        snprintf(package, sizeof(package), "%s/game/program%u", root,
                 programs[i]);
        write_control_files(package, choice, base_only);
    }
}

static Result mount_nup_source(const NupInput *input, NupMount *mount,
                               void *user) {
    NupMountContext *context = user;
    unmount_nup_source(context);
    if (input->source_id >= (u32)g.choice_count)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    ContentChoice *choice = &g.choices[input->source_id];
    NsApplicationContentMetaStatus *status = &g.meta[choice->meta_index];
    Result result = 0;
    bool base_only = false;

    if (choice->kind == CONTENT_GAME) {
        set_program_index_override(
            input->program_index == PROGRAM_INDEX_AUTO
                ? -1 : input->program_index);
        bool exefs_base_only = false;
        result = mount_nup_combined_exefs(choice, context, &exefs_base_only);
        if (R_SUCCEEDED(result)) {
            const bool was_base_only = g.romfs_base_only;
            g.romfs_base_only = false;
            bool romfs_device = false;
            const u64 application_id =
                g.applications[g.selected].record.application_id;
            if (exefs_base_only) {
                /* The update's RomFS would not match the base ExeFS. */
                log_line("romfs-warning base-only: the update ExeFS is unavailable; using the base RomFS to match");
                result = mount_base_title_romfs(application_id,
                                                &romfs_device);
                g.romfs_base_only = true;
                base_only = true;
            } else {
                result = mount_title_romfs(application_id, &romfs_device);
            }
            const bool romfs_base_only = g.romfs_base_only;
            g.romfs_base_only = was_base_only || romfs_base_only;
            if (R_SUCCEEDED(result)) {
                context->romfs_active = true;
                context->romfs_device = romfs_device;
                context->romfs_combined_name = true;
                snprintf(mount->romfs, sizeof(mount->romfs), "kdrom:/");
            }
            if (R_SUCCEEDED(result) && romfs_base_only && !exefs_base_only &&
                choice->update_index >= 0) {
                /* Base RomFS with the update's ExeFS would be a mismatched
                   game, so export the base version consistently. */
                log_line("exefs-warning base-only: the update RomFS is unavailable; using the base ExeFS to match");
                unmount_nup_exefs(context);
                result = mount_nup_exefs_status(
                    &g.meta[choice->meta_index], NULL,
                    g.applications[g.selected].record.application_id,
                    context);
                if (R_SUCCEEDED(result)) g.exefs_base_only = true;
                base_only = true;
            }
        }
        set_program_index_override(-1);
    } else if (choice->kind == CONTENT_BASE) {
        set_program_index_override(
            input->program_index == PROGRAM_INDEX_AUTO
                ? -1 : input->program_index);
        const u64 application_id =
            g.applications[g.selected].record.application_id;
        result = mount_nup_exefs_status(status, NULL, application_id, context);
        if (R_SUCCEEDED(result)) {
            bool romfs_device = false;
            if (input->program_index != PROGRAM_INDEX_AUTO &&
                input->program_index > 0) {
                result = mount_base_program_romfs(
                    application_id, status, &romfs_device);
                if (R_SUCCEEDED(result)) {
                    context->romfs_active = true;
                    context->romfs_device = romfs_device;
                    context->romfs_combined_name = true;
                    snprintf(mount->romfs, sizeof(mount->romfs), "kdrom:/");
                } else if (result ==
                           MAKERESULT(Module_Libnx, LibnxError_NotFound)) {
                    const NsApplicationContentMetaStatus *merged_update = NULL;
                    result = mount_nup_romfs_status(
                        status, context, &merged_update);
                    if (R_SUCCEEDED(result))
                        snprintf(mount->romfs, sizeof(mount->romfs),
                                 "kdnrom:/");
                }
            } else {
                const NsApplicationContentMetaStatus *merged_update = NULL;
                result = mount_nup_romfs_status(
                    status, context, &merged_update);
                if (R_SUCCEEDED(result))
                    snprintf(mount->romfs, sizeof(mount->romfs), "kdnrom:/");
            }
        }
        set_program_index_override(-1);
    } else {
        const NsApplicationContentMetaStatus *merged_update = NULL;
        result = mount_nup_romfs_status(status, context, &merged_update);
        if (R_SUCCEEDED(result))
            snprintf(mount->romfs, sizeof(mount->romfs), "kdnrom:/");
        if (R_SUCCEEDED(result) && merged_update) {
            /* The package is the DLC as updated. */
            mount->version = merged_update->version;
            mount->version_set = true;
        }
    }
    if (R_SUCCEEDED(result) && context->exefs_active)
        snprintf(mount->exefs, sizeof(mount->exefs), "kdnexe:/");
    if (R_SUCCEEDED(result)) attach_nup_meta(choice, base_only, mount);
    if (R_SUCCEEDED(result) && base_only && choice->update_index >= 0) {
        /* The package holds the base game, not the update's version. */
        mount->version = g.meta[choice->meta_index].version;
        mount->version_set = true;
    }
    if (R_FAILED(result)) unmount_nup_source(context);
    log_line("nup-source-mount choice=%u kind=%u id=%016llX result=0x%08x exefs=%s romfs=%s",
             input->source_id, choice->kind,
             (unsigned long long)input->title_id, result,
             mount->exefs[0] ? mount->exefs : "-",
             mount->romfs[0] ? mount->romfs : "-");
    return result;
}

typedef struct {
    PadState *pad;
    const char *message;
} NupUiContext;

/* Set when an FTP server itself fails (start-up or socket error), as
   opposed to the dump being prepared. */
static bool g_ftp_server_failed;

/* Shared progress callbacks --------------------------------------------- */

static bool nup_cancelled(void *user) {
    NupUiContext *context = user;
    progress_tick(g.progress_total_bytes ? context->message : NULL);
    return dump_cancelled(context->pad);
}

/* A B press is reported once (edge-triggered), so remember it: every later
   check in the same server session must also see the server as stopped. */
static bool g_ftp_stop_requested;

static bool ftp_stopped(void *user) {
    if (!g_ftp_stop_requested)
        g_ftp_stop_requested = dump_cancelled((PadState *)user);
    return g_ftp_stop_requested;
}

/* Extracted-file FTP counts files sent; archive FTP keeps the archive's
   own file count for the result screen. */
static bool g_ftp_counts_files;

static void ftp_status(void *user, const char *status, uint64_t transferred,
                       uint64_t files) {
    (void)user;
    if (status) {
        log_line("ftp-status message=%s transferred=%llu files=%llu", status,
                 (unsigned long long)transferred, (unsigned long long)files);
        if (!strncmp(status, "ftp://", 6))
            snprintf(g.ftp_address, sizeof(g.ftp_address), "%s", status);
        else
            snprintf(g.status, sizeof(g.status), "%s", status);
    }
    g.copied_bytes = transferred;
    if (g_ftp_counts_files) g.copied_files = files;
    g.screen = SCREEN_FTP;
    draw_ftp();
}

static size_t build_nup_inputs(NupInput *inputs, size_t capacity) {
    Application *app = &g.applications[g.selected];
    /* NUP SPEC §3.1.2 allows one base package per container. Game already
       contains the base, so a NUP skips a separately selected Base game;
       extracted and TAR output keep both in game/ and base/. */
    bool game_selected = false;
    for (s32 i = 0; i < g.choice_count; ++i)
        if (g.choices[i].selected && g.choices[i].kind == CONTENT_GAME)
            game_selected = true;
    size_t count = 0;
    for (s32 i = 0; i < g.choice_count && count < capacity; ++i) {
        ContentChoice *choice = &g.choices[i];
        if (!choice->selected) continue;
        if (choice->kind == CONTENT_BASE && game_selected &&
            g.output_format == OUTPUT_NUP) {
            log_line("nup-input-skip-base reason=game-selected");
            continue;
        }
        NsApplicationContentMetaStatus *meta = &g.meta[choice->meta_index];
        NupInput *input = &inputs[count];
        memset(input, 0, sizeof(*input));
        input->source_id = (u32)i;
        input->program_index = PROGRAM_INDEX_AUTO;
        if (choice->kind == CONTENT_GAME || choice->kind == CONTENT_BASE) {
            const bool is_game = choice->kind == CONTENT_GAME;
            const NsApplicationContentMetaStatus *program_status =
                is_game && choice->update_index >= 0
                    ? &g.meta[choice->update_index] : meta;
            const u64 default_title_id = is_game
                ? app->record.application_id : meta->application_id;
            const u32 version = program_status->version;
            const u8 base_layout = is_game
                ? NUP_LAYOUT_GAME : NUP_LAYOUT_BASE;
            const u8 extra_layout = is_game
                ? NUP_LAYOUT_GAME_PROGRAM : NUP_LAYOUT_BASE_PROGRAM;

            GameProgramList programs;
            load_status_programs(program_status, &programs);

            ProgramPlanInput planned[MAX_GAME_PROGRAMS];
            size_t max_planned = capacity - count;
            if (max_planned > MAX_GAME_PROGRAMS)
                max_planned = MAX_GAME_PROGRAMS;
            u8 chosen_program = PROGRAM_INDEX_AUTO;
            size_t planned_count = plan_program_inputs(
                planned, max_planned, (u32)i, default_title_id, version,
                NUP_KIND_BASE, base_layout, extra_layout,
                g.output_format == OUTPUT_NUP, &programs,
                g.nup_program_index, &chosen_program);
            if (g.output_format == OUTPUT_NUP && programs.count > 1 &&
                planned_count) {
                log_line("nup-input-select-program kind=%s index=%u title=%016llX count=%d",
                         is_game ? "game" : "base", chosen_program,
                         (unsigned long long)planned[0].title_id,
                         programs.count);
            }
            for (size_t p = 0; p < planned_count && count < capacity; ++p) {
                NupInput *slot = &inputs[count++];
                memset(slot, 0, sizeof(*slot));
                slot->title_id = planned[p].title_id;
                slot->version = planned[p].version;
                slot->source_id = planned[p].source_id;
                slot->kind = planned[p].kind;
                slot->layout = planned[p].layout;
                slot->program_index = planned[p].program_index;
            }
            continue;
        } else {
            input->title_id = meta->application_id;
            input->version = meta->version;
            input->kind = NUP_KIND_ADDON;
            input->layout = NUP_LAYOUT_DLC;
        }
        ++count;
    }
    return count;
}

static Result nup_ftp_read(void *file, uint64_t offset, void *buffer,
                           size_t size, size_t *bytes_read) {
    return nupArchiveRead((NupArchive *)file, offset, buffer, size, bytes_read);
}

static Result tar_ftp_read(void *file, uint64_t offset, void *buffer,
                           size_t size, size_t *bytes_read) {
    return tarArchiveRead((TarArchive *)file, offset, buffer, size, bytes_read);
}

static Result create_selected_nup(const char *root, PadState *pad) {
    Application *app = &g.applications[g.selected];
    NupInput inputs[MAX_META_STATUS];
    size_t count = build_nup_inputs(inputs, MAX_META_STATUS);
    if (!count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (ensure_directory(root) != 0)
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    char output[576];
    char name[256];
    build_output_name(app, ".nup", name, sizeof(name));
    snprintf(output, sizeof(output), "%s/%s", root, name);
    NupMountContext mount_context = {0};
    NupUiContext ui_context = {
        .pad = pad,
        .message = "Writing NUP directly to SD...",
    };
    draw_progress("Inventorying NUP contents...");
    Result result = nupCreateDirect(
        output, inputs, count, mount_nup_source, unmount_nup_source,
        &mount_context, g_copy_buffer, COPY_BUFFER_SIZE,
        nup_cancelled, &ui_context, &g.progress_total_bytes,
        &g.copied_bytes, &g.copied_files);
    log_line("nup-create path=%s result=0x%08x packages=%zu files=%llu bytes=%llu",
             output, result, count, (unsigned long long)g.copied_files,
             (unsigned long long)g.copied_bytes);
    return result;
}

/* NUP output ------------------------------------------------------------- */

static Result serve_selected_nup(PadState *pad) {
    Application *app = &g.applications[g.selected];
    NupInput inputs[MAX_META_STATUS];
    size_t count = build_nup_inputs(inputs, MAX_META_STATUS);
    if (!count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    NupMountContext mount_context = {0};
    NupUiContext ui_context = {
        .pad = pad,
        .message = "Hashing content for virtual NUP...",
    };
    NupArchive *archive = NULL;
    draw_progress("Inventorying virtual NUP contents...");
    Result result = nupPrepareVirtual(
        &archive, inputs, count, mount_nup_source, unmount_nup_source,
        &mount_context, g_copy_buffer, COPY_BUFFER_SIZE,
        nup_cancelled, &ui_context, &g.progress_total_bytes,
        &g.copied_bytes, &g.copied_files);
    log_line("nup-virtual-prepare result=0x%08x packages=%zu files=%llu hashed=%llu size=%llu",
             result, count, (unsigned long long)g.copied_files,
             (unsigned long long)g.copied_bytes,
             (unsigned long long)nupArchiveSize(archive));
    if (R_FAILED(result)) return result;

    char name[256];
    build_output_name(app, ".nup", name, sizeof(name));
    g.copied_bytes = 0;
    g.ftp_address[0] = 0;
    snprintf(g.status, sizeof(g.status), "Starting virtual read-only FTP...");
    g.screen = SCREEN_FTP;
    draw_ftp();
    result = ftpServeVirtualFile(
        name, nupArchiveSize(archive), archive, nup_ftp_read, 5000,
        ftp_stopped, ftp_status, pad);
    if (R_FAILED(result)) g_ftp_server_failed = true;
    log_line("ftp-finish virtual=%s result=0x%08x transferred=%llu",
             name, result, (unsigned long long)g.copied_bytes);
    nupArchiveClose(archive);
    g.screen = SCREEN_RESULT;
    if (R_SUCCEEDED(result))
        snprintf(g.status, sizeof(g.status),
                 "FTP server stopped; no dump data was written to SD");
    return result;
}

/* USB MTP ---------------------------------------------------------------- */

static void usb_worker_main(void *argument) {
    mtp_run();
    nupArchiveDeactivate((NupArchive *)argument);
}

static void log_usb_activity(void) {
    app_status_t status;
    status_snapshot(&status);
    log_line("usb-status state=%u speed-mps=%u done=%llu total=%llu to-pc=%llu files=%u error=%s",
             status.state, status.usb_speed_mps,
             (unsigned long long)status.done,
             (unsigned long long)status.total,
             (unsigned long long)status.bytes_to_pc,
             status.files_to_pc,
             status.error[0] ? status.error : "-");
    u32 first = status.log_head > STATUS_LOG_LINES
        ? status.log_head - STATUS_LOG_LINES : 0;
    for (u32 i = first; i < status.log_head; ++i)
        log_line("usb-activity %s", status.log[i % STATUS_LOG_LINES]);
}

static Result run_usb_server(PadState *pad, NupArchive *archive,
                             const char *root_path, const char *description,
                             void (*unmount_virtual)(void)) {
    Result result = 0;
    status_init();
    g_usb_stop = 0;
    tp_set_stop_flag(&g_usb_stop);
    tp_set_serial("KAIDUMP0001");
    const mtp_config_t config = {
        .root_path = root_path,
        .storage_desc = description,
        .manufacturer = "KaiDump contributors",
        .model = "KaiDump",
        .friendly_name = "KaiDump",
        .version = "KaiDump " KAIDUMP_VERSION,
        .serial = "KAIDUMP0001",
        .stop = &g_usb_stop,
    };

    bool mtp_ready = mtp_init(&config);
    log_line("usb-mtp-init ready=%u", mtp_ready ? 1u : 0u);
    bool transport_ready = false;
    bool thread_created = false;
    bool thread_running = false;
    if (!mtp_ready) {
        result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        status_set_error("Could not allocate the USB transfer buffers.");
    } else if (tp_init() != TP_OK) {
        result = tp_last_result();
        if (R_SUCCEEDED(result))
            result = MAKERESULT(Module_Libnx, LibnxError_IoError);
        log_line("usb-transport-init step=%s result=0x%08x",
                 tp_last_step(), result);
        status_set_error("USB setup failed at %s (0x%08x).",
                         tp_last_step(), result);
    } else {
        transport_ready = true;
        log_line("usb-transport-init step=%s result=0x%08x",
                 tp_last_step(), tp_last_result());
        result = threadCreate(&g_usb_thread, usb_worker_main, archive, NULL,
                              0x20000, 0x2B, -2);
        thread_created = R_SUCCEEDED(result);
        if (thread_created) {
            result = threadStart(&g_usb_thread);
            thread_running = R_SUCCEEDED(result);
        }
        if (R_FAILED(result))
            status_set_error("Could not start the USB transfer thread (0x%08x).",
                             result);
    }

    if (thread_running) {
        appletSetMediaPlaybackState(true);
        g.copied_bytes = 0;
        g.screen = SCREEN_USB;
        draw_usb();
        while (appletMainLoop()) {
            uiPumpEvents();
            padUpdate(pad);
            if (buttons_pressed(pad) & HidNpadButton_B) break;
            app_status_t status;
            status_snapshot(&status);
            g.copied_bytes = status.bytes_to_pc;
            if (status.state == ST_TRANSFER)
                g.copied_bytes += status.done;
            draw_usb();
            svcSleepThread(16000000ull);
        }
        g_usb_stop = 1;
        log_line("usb-shutdown stop-requested");
        threadWaitForExit(&g_usb_thread);
        thread_running = false;
        log_line("usb-shutdown worker-exited");
        log_usb_activity();
        result = 0;
    }

    g_usb_stop = 1;
    if (thread_running) threadWaitForExit(&g_usb_thread);
    if (thread_created) {
        threadClose(&g_usb_thread);
        log_line("usb-shutdown thread-closed");
    }
    if (transport_ready) {
        tp_exit();
        log_line("usb-shutdown transport-closed");
    }
    if (mtp_ready) {
        mtp_exit();
        log_line("usb-shutdown mtp-closed");
    }
    unmount_virtual();
    log_line("usb-shutdown virtual-output-unmounted");
    nupArchiveClose(archive);
    log_line("usb-shutdown archive-closed");
    appletSetMediaPlaybackState(false);
    g.screen = SCREEN_RESULT;
    if (R_SUCCEEDED(result))
        snprintf(g.status, sizeof(g.status),
                 "USB server stopped; no dump data was written to SD");
    return result;
}

static Result serve_selected_usb(PadState *pad) {
    Application *app = &g.applications[g.selected];
    NupInput inputs[MAX_META_STATUS];
    size_t count = build_nup_inputs(inputs, MAX_META_STATUS);
    if (!count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    NupMountContext mount_context = {0};
    NupUiContext ui_context = {
        .pad = pad,
        .message = "Hashing content for virtual USB NUP...",
    };
    NupArchive *archive = NULL;
    draw_progress("Inventorying virtual USB NUP contents...");
    Result result = nupPrepareVirtual(
        &archive, inputs, count, mount_nup_source, unmount_nup_source,
        &mount_context, g_copy_buffer, COPY_BUFFER_SIZE,
        nup_cancelled, &ui_context, &g.progress_total_bytes,
        &g.copied_bytes, &g.copied_files);
    log_line("nup-usb-prepare result=0x%08x packages=%zu files=%llu hashed=%llu size=%llu",
             result, count, (unsigned long long)g.copied_files,
             (unsigned long long)g.copied_bytes,
             (unsigned long long)nupArchiveSize(archive));
    if (R_FAILED(result)) return result;

    char name[256];
    build_output_name(app, ".nup", name, sizeof(name));
    result = virtualFileMount(
        "kdnup", name, nupArchiveSize(archive), archive, nup_ftp_read);
    log_result("usb-virtual-file-mount", result);
    if (R_FAILED(result)) {
        nupArchiveClose(archive);
        return result;
    }
    return run_usb_server(pad, archive, "kdnup:",
                          "KaiDump virtual NUP", virtualFileUnmount);
}

static Result prepare_selected_tree(PadState *pad, NupArchive **archive,
                                    NupMountContext *mount_context,
                                    const char *message) {
    NupInput inputs[MAX_META_STATUS];
    size_t count = build_nup_inputs(inputs, MAX_META_STATUS);
    if (!count) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    NupUiContext ui_context = {
        .pad = pad,
        .message = message,
    };
    g.copied_files = 0;
    draw_progress(message);
    Result result = nupPrepareTree(
        archive, inputs, count, mount_nup_source, unmount_nup_source,
        mount_context, nup_cancelled, &ui_context, &g.copied_files);
    log_line("content-tree-prepare result=0x%08x packages=%zu files=%llu size=%llu",
             result, count, (unsigned long long)g.copied_files,
             (unsigned long long)nupTreeTotalSize(*archive));
    return result;
}

static Result serve_selected_extracted_ftp(PadState *pad) {
    NupMountContext mount_context = {0};
    NupArchive *archive = NULL;
    Result result = prepare_selected_tree(
        pad, &archive, &mount_context, "Inventorying extracted files...");
    if (R_FAILED(result)) return result;
    result = virtualTreeMount("kdtree", archive);
    log_result("ftp-virtual-tree-mount", result);
    if (R_FAILED(result)) {
        nupArchiveClose(archive);
        return result;
    }

    g.copied_bytes = 0;
    g.copied_files = 0;
    g_ftp_counts_files = true;
    g.ftp_address[0] = 0;
    snprintf(g.status, sizeof(g.status), "Starting extracted-file FTP...");
    g.screen = SCREEN_FTP;
    draw_ftp();
    result = ftpServeDirectory("kdtree:", 5000, ftp_stopped,
                               ftp_status, pad);
    if (R_FAILED(result)) g_ftp_server_failed = true;
    g_ftp_counts_files = false;
    log_line("ftp-finish extracted result=0x%08x transferred=%llu files=%llu",
             result, (unsigned long long)g.copied_bytes,
             (unsigned long long)g.copied_files);
    virtualTreeUnmount();
    nupArchiveClose(archive);
    g.screen = SCREEN_RESULT;
    if (R_SUCCEEDED(result))
        snprintf(g.status, sizeof(g.status),
                 "FTP server stopped; no dump data was written to SD");
    return result;
}

static Result serve_selected_extracted_usb(PadState *pad) {
    NupMountContext mount_context = {0};
    NupArchive *archive = NULL;
    Result result = prepare_selected_tree(
        pad, &archive, &mount_context, "Inventorying extracted files...");
    if (R_FAILED(result)) return result;
    result = virtualTreeMount("kdtree", archive);
    log_result("usb-virtual-tree-mount", result);
    if (R_FAILED(result)) {
        nupArchiveClose(archive);
        return result;
    }
    return run_usb_server(pad, archive, "kdtree:",
                          "KaiDump extracted files", virtualTreeUnmount);
}

static Result prepare_selected_tar(PadState *pad, NupArchive **tree,
                                   TarArchive **tar,
                                   NupMountContext *mount_context) {
    Result result = prepare_selected_tree(
        pad, tree, mount_context, "Inventorying TAR contents...");
    if (R_SUCCEEDED(result)) result = tarPrepare(tar, *tree);
    log_line("tar-prepare result=0x%08x files=%llu size=%llu",
             result, (unsigned long long)g.copied_files,
             (unsigned long long)tarArchiveSize(*tar));
    if (R_FAILED(result) && *tree) {
        nupArchiveClose(*tree);
        *tree = NULL;
    }
    return result;
}

static Result create_selected_tar(const char *root, PadState *pad) {
    Application *app = &g.applications[g.selected];
    if (ensure_directory(root) != 0)
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    NupMountContext mount_context = {0};
    NupArchive *tree = NULL;
    TarArchive *tar = NULL;
    Result result = prepare_selected_tar(pad, &tree, &tar, &mount_context);
    if (R_FAILED(result)) return result;

    char output[576];
    char name[256];
    build_output_name(app, ".tar", name, sizeof(name));
    snprintf(output, sizeof(output), "%s/%s", root, name);
    NupUiContext ui_context = {
        .pad = pad,
        .message = "Writing TAR directly to SD...",
    };
    progress_set_labels("FILES IN ARCHIVE", "ARCHIVE WRITTEN");
    g.copied_bytes = 0;
    g.progress_total_bytes = tarArchiveSize(tar);
    draw_progress(ui_context.message);
    result = tarWriteDirect(output, tar, g_copy_buffer, COPY_BUFFER_SIZE,
                            nup_cancelled, &ui_context, &g.copied_bytes);
    log_line("tar-create path=%s result=0x%08x files=%llu bytes=%llu",
             output, result, (unsigned long long)g.copied_files,
             (unsigned long long)g.copied_bytes);
    tarArchiveClose(tar);
    nupArchiveClose(tree);
    return result;
}

static Result serve_selected_tar_ftp(PadState *pad) {
    Application *app = &g.applications[g.selected];
    NupMountContext mount_context = {0};
    NupArchive *tree = NULL;
    TarArchive *tar = NULL;
    Result result = prepare_selected_tar(pad, &tree, &tar, &mount_context);
    if (R_FAILED(result)) return result;
    progress_set_labels("FILES IN ARCHIVE", "DATA TRANSFERRED");

    char name[256];
    build_output_name(app, ".tar", name, sizeof(name));
    g.copied_bytes = 0;
    g.ftp_address[0] = 0;
    snprintf(g.status, sizeof(g.status), "Starting virtual read-only TAR FTP...");
    g.screen = SCREEN_FTP;
    draw_ftp();
    result = ftpServeVirtualFile(
        name, tarArchiveSize(tar), tar, tar_ftp_read, 5000,
        ftp_stopped, ftp_status, pad);
    if (R_FAILED(result)) g_ftp_server_failed = true;
    log_line("ftp-finish virtual-tar=%s result=0x%08x transferred=%llu",
             name, result, (unsigned long long)g.copied_bytes);
    tarArchiveClose(tar);
    nupArchiveClose(tree);
    g.screen = SCREEN_RESULT;
    if (R_SUCCEEDED(result))
        snprintf(g.status, sizeof(g.status),
                 "FTP server stopped; no dump data was written to SD");
    return result;
}

static Result serve_selected_tar_usb(PadState *pad) {
    Application *app = &g.applications[g.selected];
    NupMountContext mount_context = {0};
    NupArchive *tree = NULL;
    TarArchive *tar = NULL;
    Result result = prepare_selected_tar(pad, &tree, &tar, &mount_context);
    if (R_FAILED(result)) return result;
    progress_set_labels("FILES IN ARCHIVE", "DATA TRANSFERRED");

    char name[256];
    build_output_name(app, ".tar", name, sizeof(name));
    result = virtualFileMount(
        "kdtar", name, tarArchiveSize(tar), tar, tar_ftp_read);
    log_result("usb-virtual-tar-mount", result);
    if (R_FAILED(result)) {
        tarArchiveClose(tar);
        nupArchiveClose(tree);
        return result;
    }
    result = run_usb_server(pad, tree, "kdtar:",
                            "KaiDump virtual TAR", virtualFileUnmount);
    tarArchiveClose(tar);
    return result;
}

static bool is_switch2_title(u64 application_id) {
    return is_switch2_program_id(application_id);
}

void dump_title(PadState *pad) {
    Application *app = &g.applications[g.selected];
    g.copied_bytes = 0;
    g.copied_files = 0;
    sourceIoResetError();
    g.status[0] = 0;
    g.screen = SCREEN_RESULT;
    g.romfs_base_only = false;
    g_ftp_server_failed = false;
    free_nup_meta();
    g_ftp_stop_requested = false;
    g.exefs_base_only = false;
    g.romfs_self_matched = false;
    g.dlc_update_missing = false;

    log_line("export-start title=%016llX name=%s mode=%s destination=%s choices=%d selected=%d",
             (unsigned long long)app->record.application_id,
             app->title[0] ? app->title : "(unavailable)",
             g.output_format == OUTPUT_NUP ? "nup"
                 : g.output_format == OUTPUT_TAR ? "tar" : "extracted",
             g.usb_enabled ? "usb" : g.ftp_enabled ? "ftp" : "sd",
             g.choice_count,
             selected_content_count());
    for (s32 i = 0; i < g.choice_count; ++i) {
        const ContentChoice *choice = &g.choices[i];
        const NsApplicationContentMetaStatus *meta =
            choice->meta_index >= 0 && choice->meta_index < g.meta_count
                ? &g.meta[choice->meta_index] : NULL;
        const NsApplicationContentMetaStatus *update =
            choice->update_index >= 0 && choice->update_index < g.meta_count
                ? &g.meta[choice->update_index] : NULL;
        log_line("export-choice index=%d selected=%u kind=%u meta-index=%d update-index=%d id=%016llX meta-type=%u storage=%u version=%u update-id=%016llX update-storage=%u update-version=%u",
                 i, choice->selected, choice->kind, choice->meta_index,
                 choice->update_index,
                 (unsigned long long)(meta ? meta->application_id : 0),
                 meta ? meta->meta_type : 0, meta ? meta->storageID : 0,
                 meta ? meta->version : 0,
                 (unsigned long long)(update ? update->application_id : 0),
                 update ? update->storageID : 0,
                 update ? update->version : 0);
    }

    if (!hosversionIsKaiHEN()) {
        g.dump_result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        snprintf(g.status, sizeof(g.status), "kaiHEN was not detected; dump refused");
        log_line("romfs-refused title=%016llX reason=no-kaiHEN",
                 (unsigned long long)app->record.application_id);
        return;
    }
    if (g.ftp_enabled) {
        refresh_ftp_availability();
        if (!g.ftp_available) {
            g.dump_result = g.ftp_network_result;
            snprintf(g.status, sizeof(g.status),
                     "FTP unavailable; connect the console to a local network");
            log_result("ftp-network-required", g.dump_result);
            return;
        }
    }
    g_copy_buffer = malloc(COPY_BUFFER_SIZE);
    if (!g_copy_buffer) {
        g.dump_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        snprintf(g.status, sizeof(g.status), "Could not allocate the 1 MiB copy buffer");
        log_line("romfs-refused title=%016llX reason=out-of-memory",
                 (unsigned long long)app->record.application_id);
        return;
    }

    progress_begin("Preparing selected content...");

    char root[320];
    char output_name[256];
    build_output_name(app, "", output_name, sizeof(output_name));
    snprintf(root, sizeof(root),
             "sdmc:/switch/KaiDump/dumps/%s", output_name);
    g.dump_result = 0;
    int completed = selected_content_count();
    if (g.usb_enabled) {
        g.dump_result = g.output_format == OUTPUT_NUP
            ? serve_selected_usb(pad)
            : g.output_format == OUTPUT_TAR
                ? serve_selected_tar_usb(pad)
                : serve_selected_extracted_usb(pad);
    } else if (g.ftp_enabled) {
        g.dump_result = g.output_format == OUTPUT_NUP
            ? serve_selected_nup(pad)
            : g.output_format == OUTPUT_TAR
                ? serve_selected_tar_ftp(pad)
                : serve_selected_extracted_ftp(pad);
    } else if (g.output_format == OUTPUT_NUP) {
        g.dump_result = create_selected_nup(root, pad);
    } else if (g.output_format == OUTPUT_TAR) {
        g.dump_result = create_selected_tar(root, pad);
    } else {
        completed = 0;
        NupMountContext measure_context = {0};
        NupArchive *measure_tree = NULL;
        g.dump_result = prepare_selected_tree(
            pad, &measure_tree, &measure_context,
            "Inventorying extracted files...");
        if (R_SUCCEEDED(g.dump_result)) {
            g.progress_total_bytes = nupTreeTotalSize(measure_tree);
            nupArchiveClose(measure_tree);
            g.copied_bytes = 0;
            g.copied_files = 0;
            draw_progress("Starting extracted copy...");
            for (s32 i = 0; i < g.choice_count; ++i) {
                ContentChoice *choice = &g.choices[i];
                if (!choice->selected) continue;
                const bool was_base_only = g.romfs_base_only;
                g.romfs_base_only = false;
                Result result = choice->kind == CONTENT_GAME
                    ? export_combined_game(app->record.application_id, root, pad)
                    : export_selected_content(&g.meta[choice->meta_index], root, pad);
                const bool base_only = g.romfs_base_only;
                g.romfs_base_only = was_base_only || base_only;
                if (R_FAILED(result)) {
                    g.dump_result = result;
                    break;
                }
                write_selected_control_files(root, choice, base_only);
                ++completed;
            }
        }
    }
    g.progress_active = false;
    if (R_SUCCEEDED(g.dump_result)) {
        if (!g.ftp_enabled && !g.usb_enabled)
            snprintf(g.status, sizeof(g.status),
                     g.output_format == OUTPUT_NUP
                         ? "%d selected content package%s archived as NUP"
                         : g.output_format == OUTPUT_TAR
                             ? "%d selected content package%s archived as TAR"
                             : "%d selected content package%s exported successfully",
                     completed, completed == 1 ? "" : "s");
        if (g.romfs_base_only && g.exefs_base_only)
            snprintf(g.status, sizeof(g.status),
                     is_switch2_title(app->record.application_id) &&
                             !g.romfs_self_matched
                         ? "Update unavailable, so the base version was exported. Launch kaiHEN through this game to include it"
                         : "Warning: the update could not be opened, so the base version was exported (see KaiDump.log)");
        else if (g.romfs_base_only)
            snprintf(g.status, sizeof(g.status),
                     "Warning: the update's RomFS was unavailable, so the base version was used (see KaiDump.log)");
        else if (g.exefs_base_only)
            snprintf(g.status, sizeof(g.status),
                     "Warning: the update's ExeFS was unavailable, so the base ExeFS was used (see KaiDump.log)");
        else if (g.dlc_update_missing)
            snprintf(g.status, sizeof(g.status),
                     "Warning: a DLC update could not be merged, so that DLC was exported without it (see KaiDump.log)");
    } else if (R_MODULE(g.dump_result) == Module_Libnx &&
               R_DESCRIPTION(g.dump_result) == KAIDUMP_ERROR_CANCELED) {
        snprintf(g.status, sizeof(g.status),
                 (g.ftp_enabled || g.usb_enabled)
                     ? "Cancelled; no dump data was written to SD"
                     : "Cancelled; partial output was kept");
    } else if (sourceIoHadReadError()) {
        snprintf(g.status, sizeof(g.status),
                 "RomFS read failed; retry or verify the installed game data");
    } else if (R_MODULE(g.dump_result) == Module_Libnx &&
               R_DESCRIPTION(g.dump_result) == TAR_ERROR_FAT32_LIMIT) {
        snprintf(g.status, sizeof(g.status),
                 "The SD card is FAT32 and cannot store this TAR; use FTP, USB, or Extracted Files");
    } else if (R_MODULE(g.dump_result) == Module_Libnx &&
               R_DESCRIPTION(g.dump_result) ==
                   KAIDUMP_ERROR_FAT32_FILE_LIMIT) {
        snprintf(g.status, sizeof(g.status),
                 "The SD card cannot store this file because it is larger than 4 GiB; use exFAT, FTP, or USB");
    } else {
        char code[16];
        format_result_code(code, sizeof(code), g.dump_result);
        if (R_VALUE(g.dump_result) == 0x7d802) {
            if (is_switch2_title(app->record.application_id) &&
                !g.romfs_self_matched) {
                snprintf(g.status, sizeof(g.status),
                         "Dump failed. Try launching kaiHEN through the game you are trying to dump, then open KaiDump and retry (%s)",
                         code);
                log_line("export-retry-hint title=%016llX reason=NcaExternalKeyNotFound switch2=1 self-match=0",
                         (unsigned long long)app->record.application_id);
            } else if (g.romfs_self_matched) {
                snprintf(g.status, sizeof(g.status),
                         "Content key unavailable (%s) even inside the game; see KaiDump.log",
                         code);
            } else {
                snprintf(g.status, sizeof(g.status),
                         "Content key unavailable (%s); see KaiDump.log",
                         code);
            }
        } else if (g_ftp_server_failed) {
            snprintf(g.status, sizeof(g.status),
                     g.ftp_address[0]
                         ? "FTP server stopped with an error (%s); see KaiDump.log"
                         : "FTP server could not start (%s); see KaiDump.log",
                     code);
        } else if (g.ftp_enabled || g.usb_enabled) {
            /* Nothing is written to SD for FTP or USB, so nothing was kept. */
            snprintf(g.status, sizeof(g.status),
                     "Export failed (%s); nothing was written to SD", code);
        } else if (!g.copied_files && !g.copied_bytes) {
            snprintf(g.status, sizeof(g.status),
                     "Export failed (%s); nothing was written", code);
        } else {
            snprintf(g.status, sizeof(g.status),
                     "One content section failed (%s); successful output was kept",
                     code);
        }
    }
    free(g_copy_buffer);
    g_copy_buffer = NULL;
    free_nup_meta();
    log_line("export-finish result=0x%08x packages=%d files=%llu bytes=%llu status=%s",
             g.dump_result, completed, (unsigned long long)g.copied_files,
             (unsigned long long)g.copied_bytes, g.status);
}
