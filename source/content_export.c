#include "app_internal.h"

/* Filesystem walking and extracted-file output. */

static Result copy_tree(const char *source, const char *destination,
                        const char *relative, PadState *pad) {
    DIR *directory = opendir(source);
    if (!directory) {
        log_line("copy-opendir source=%s errno=%d", source, errno);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }
    int directory_error = ensure_directory(destination);
    if (directory_error != 0) {
        log_line("copy-mkdir destination=%s errno=%d", destination,
                 directory_error);
        closedir(directory);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    Result result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char src[1024], dst[1024], rel[768];
        const char *src_separator = source[strlen(source) - 1] == '/' ? "" : "/";
        const char *dst_separator = destination[strlen(destination) - 1] == '/' ? "" : "/";
        int a = snprintf(src, sizeof(src), "%s%s%s", source, src_separator,
                         entry->d_name);
        int b = snprintf(dst, sizeof(dst), "%s%s%s", destination, dst_separator,
                         entry->d_name);
        int c = relative[0]
                    ? snprintf(rel, sizeof(rel), "%s/%s", relative, entry->d_name)
                    : snprintf(rel, sizeof(rel), "%s", entry->d_name);
        if (a < 0 || b < 0 || c < 0 || (size_t)a >= sizeof(src) ||
            (size_t)b >= sizeof(dst) || (size_t)c >= sizeof(rel)) {
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }

        struct stat st;
        if (stat(src, &st) != 0) {
            log_line("copy-stat source=%s type=%u errno=%d", src,
                     entry->d_type, errno);
            result = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }
        bool is_directory = S_ISDIR(st.st_mode);
        bool is_regular = S_ISREG(st.st_mode);
        if (is_directory) {
            result = copy_tree(src, dst, rel, pad);
            if (R_FAILED(result)) break;
            continue;
        }
        if (!is_regular) {
            log_line("copy-skip source=%s type=%u", src, entry->d_type);
            continue;
        }

        FILE *in = fopen(src, "rb");
        FILE *out = in ? fopen(dst, "wb") : NULL;
        if (!in || !out) {
            log_line("copy-open source=%s destination=%s input=%u output=%u errno=%d",
                     src, dst, in != NULL, out != NULL, errno);
            if (in) fclose(in);
            if (out) fclose(out);
            result = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }
        setvbuf(in, NULL, _IONBF, 0);
        draw_progress(rel);
        uint64_t offset = 0;
        while (offset < (uint64_t)st.st_size) {
            size_t wanted = (uint64_t)st.st_size - offset > COPY_BUFFER_SIZE
                ? COPY_BUFFER_SIZE : (size_t)((uint64_t)st.st_size - offset);
            result = sourceReadExact(in, src, offset, g_copy_buffer, wanted);
            if (R_FAILED(result)) {
                log_line("copy-read source=%s offset=%llu size=%zu result=0x%08x",
                         src, (unsigned long long)offset, wanted, result);
                break;
            }
            size_t written = fwrite(g_copy_buffer, 1, wanted, out);
            if (written != wanted) {
                const int write_errno = errno;
                const uint64_t position = offset + written;
                const bool likely_fat32 =
                    (uint64_t)st.st_size > UINT32_MAX &&
                    position >= UINT32_MAX - COPY_BUFFER_SIZE;
                log_line("copy-write destination=%s source-size=%llu offset=%llu wanted=%zu written=%zu errno=%d native=0x%08x fat32-limit=%u",
                         dst, (unsigned long long)st.st_size,
                         (unsigned long long)offset, wanted, written,
                         write_errno, fsdevGetLastResult(), likely_fat32);
                result = MAKERESULT(
                    Module_Libnx,
                    likely_fat32 ? KAIDUMP_ERROR_FAT32_FILE_LIMIT
                                 : LibnxError_IoError);
                break;
            }
            offset += wanted;
            g.copied_bytes += wanted;
            if (dump_cancelled(pad)) {
                result = MAKERESULT(Module_Libnx, KAIDUMP_ERROR_CANCELED);
                break;
            }
        }
        if (fclose(out) != 0 && R_SUCCEEDED(result))
            result = MAKERESULT(Module_Libnx, LibnxError_IoError);
        fclose(in);
        if (R_FAILED(result)) break;
        ++g.copied_files;
        if (dump_cancelled(pad)) {
            result = MAKERESULT(Module_Libnx, KAIDUMP_ERROR_CANCELED);
            break;
        }
    }
    closedir(directory);
    return result;
}

static Result copy_filesystem_tree(FsFileSystem *filesystem, const char *source,
                                   const char *destination,
                                   const char *relative, PadState *pad) {
    int directory_error = ensure_directory(destination);
    if (directory_error) {
        log_line("exefs-direct-mkdir destination=%s errno=%d", destination,
                 directory_error);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    FsDir directory = {0};
    Result result = fsFsOpenDirectory(
        filesystem, source, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles,
        &directory);
    if (R_FAILED(result)) {
        log_line("exefs-direct-opendir source=%s result=0x%08x", source, result);
        return result;
    }

    for (;;) {
        FsDirectoryEntry entry = {0};
        s64 count = 0;
        result = fsDirRead(&directory, &count, 1, &entry);
        if (R_FAILED(result) || count == 0) break;
        if (!entry.name[0] || strchr(entry.name, '/') || strchr(entry.name, '\\') ||
            !strcmp(entry.name, ".") || !strcmp(entry.name, "..")) {
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }

        char src[FS_MAX_PATH], dst[1024], rel[768];
        int a = !strcmp(source, "/")
            ? snprintf(src, sizeof(src), "/%s", entry.name)
            : snprintf(src, sizeof(src), "%s/%s", source, entry.name);
        int b = snprintf(dst, sizeof(dst), "%s/%s", destination, entry.name);
        int c = relative[0]
            ? snprintf(rel, sizeof(rel), "%s/%s", relative, entry.name)
            : snprintf(rel, sizeof(rel), "%s", entry.name);
        if (a < 0 || b < 0 || c < 0 || (size_t)a >= sizeof(src) ||
            (size_t)b >= sizeof(dst) || (size_t)c >= sizeof(rel)) {
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }

        if (entry.type == FsDirEntryType_Dir) {
            result = copy_filesystem_tree(filesystem, src, dst, rel, pad);
            if (R_FAILED(result)) break;
            continue;
        }
        if (entry.type != FsDirEntryType_File) continue;
        if (entry.file_size < 0) {
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
            break;
        }

        FsFile input = {0};
        result = fsFsOpenFile(filesystem, src, FsOpenMode_Read, &input);
        FILE *output = R_SUCCEEDED(result) ? fopen(dst, "wb") : NULL;
        if (R_FAILED(result) || !output) {
            log_line("exefs-direct-open source=%s destination=%s result=0x%08x errno=%d",
                     src, dst, result, errno);
            if (serviceIsActive(&input.s)) fsFileClose(&input);
            if (output) fclose(output);
            if (R_SUCCEEDED(result))
                result = MAKERESULT(Module_Libnx, LibnxError_IoError);
            break;
        }

        draw_progress(rel);
        u64 offset = 0;
        while (offset < (u64)entry.file_size) {
            u64 wanted = (u64)entry.file_size - offset;
            if (wanted > COPY_BUFFER_SIZE) wanted = COPY_BUFFER_SIZE;
            u64 got = 0;
            result = fsFileRead(&input, (s64)offset, g_copy_buffer, wanted,
                                FsReadOption_None, &got);
            if (R_FAILED(result) || got == 0 ||
                fwrite(g_copy_buffer, 1, (size_t)got, output) != got) {
                if (R_SUCCEEDED(result))
                    result = MAKERESULT(Module_Libnx, LibnxError_IoError);
                break;
            }
            offset += got;
            g.copied_bytes += got;
            if (dump_cancelled(pad)) {
                result = MAKERESULT(Module_Libnx, KAIDUMP_ERROR_CANCELED);
                break;
            }
        }
        if (fclose(output) != 0 && R_SUCCEEDED(result))
            result = MAKERESULT(Module_Libnx, LibnxError_IoError);
        fsFileClose(&input);
        if (R_FAILED(result)) break;
        ++g.copied_files;
    }
    fsDirClose(&directory);
    return result;
}

static Result copy_selected_exefs(
    const NsApplicationContentMetaStatus *status, const char *destination,
    PadState *pad) {
    FsFileSystem filesystem = {0};
    Result result = open_title_exefs(
        &filesystem, g.applications[g.selected].record.application_id,
        status, NULL);
    if (R_SUCCEEDED(result)) {
        result = copy_filesystem_tree(
            &filesystem, "/", destination, "exefs", pad);
        close_code_filesystem(&filesystem);
    }
    return result;
}

static Result copy_selected_romfs(
    const NsApplicationContentMetaStatus *status, const char *destination,
    PadState *pad) {
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
            log_line("selected-romfs-dlc-update-merged id=%016llX version=%u",
                     (unsigned long long)status->application_id,
                     update->version);
            result = romfsMountFromStorage(storage, 0, "kdrom");
            log_line("selected-romfs-mount-storage type=%u result=0x%08x",
                     status->meta_type, result);
            if (R_SUCCEEDED(result)) {
                result = copy_tree("kdrom:/", destination, "romfs", pad);
                romfsUnmount("kdrom");
                return result;
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
        log_line("selected-romfs-open-data-id id=%016llX storage=%u result=0x%08x",
                 (unsigned long long)status->application_id,
                 status->storageID, result);
    } else {
        result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }
    if (R_FAILED(result))
        result = open_data_storage_by_path(
            &storage, path, attributes, proxy_type);
    log_line("selected-romfs-open-storage type=%u proxy=%u result=0x%08x",
             status->meta_type, proxy_type, result);
    if (R_SUCCEEDED(result)) {
        result = romfsMountFromStorage(storage, 0, "kdrom");
        log_line("selected-romfs-mount-storage type=%u result=0x%08x",
                 status->meta_type, result);
        if (R_SUCCEEDED(result)) {
            result = copy_tree("kdrom:/", destination, "romfs", pad);
            romfsUnmount("kdrom");
            return result;
        }
        fsStorageClose(&storage);
    }

    const Result storage_failure = result;
    FsFileSystem filesystem = {0};
    result = fsOpenFileSystemWithId(
        &filesystem, status->application_id,
        (FsFileSystemType)proxy_type, path, attributes);
    log_line("selected-romfs-open-filesystem type=%u proxy=%u result=0x%08x",
             status->meta_type, proxy_type, result);
    if (R_SUCCEEDED(result)) {
        result = copy_filesystem_tree(
            &filesystem, "/", destination, "romfs", pad);
        fsFsClose(&filesystem);
        return result;
    }
    return prefer_romfs_failure(storage_failure, result);
}

Result export_selected_content(
    const NsApplicationContentMetaStatus *status, const char *root,
    PadState *pad) {
    char package[512];
    if (status->meta_type == NcmContentMetaType_Application) {
        snprintf(package, sizeof(package), "%s/base", root);
    } else if (status->meta_type == NcmContentMetaType_AddOnContent ||
               status->meta_type == NcmContentMetaType_DataPatch) {
        snprintf(package, sizeof(package), "%s/dlc/%016llX/0", root,
                 (unsigned long long)status->application_id);
    } else {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    Result exefs_result = 0;
    if (status->meta_type == NcmContentMetaType_Application) {
        char destination[576];
        snprintf(destination, sizeof(destination), "%s/exefs", package);
        exefs_result = copy_selected_exefs(status, destination, pad);
    }

    bool cancelled = R_MODULE(exefs_result) == Module_Libnx &&
                     R_DESCRIPTION(exefs_result) == KAIDUMP_ERROR_CANCELED;
    Result romfs_result = exefs_result;
    if (!cancelled) {
        char destination[576];
        snprintf(destination, sizeof(destination), "%s/romfs", package);
        romfs_result = copy_selected_romfs(status, destination, pad);
    }
    Result result = R_FAILED(exefs_result) ? exefs_result : romfs_result;
    log_line("selected-content-finish id=%016llX type=%u version=%u exefs=0x%08x romfs=0x%08x result=0x%08x",
             (unsigned long long)status->application_id, status->meta_type,
             status->version, exefs_result, romfs_result, result);
    return result;
}

static Result export_combined_program(u64 application_id,
                                      const char *package, PadState *pad) {
    char destination[576];

    const NsApplicationContentMetaStatus *base =
        find_program_status(NcmContentMetaType_Application);
    const NsApplicationContentMetaStatus *update =
        find_program_status(NcmContentMetaType_Patch);

    /* Mount the RomFS first: if only the base RomFS is available, the ExeFS
       must be the base one too, or the dump mixes two game versions. */
    const bool was_base_only = g.romfs_base_only;
    g.romfs_base_only = false;
    bool romfs_device = false;
    Result romfs_result = mount_title_romfs(application_id, &romfs_device);
    bool romfs_mounted = R_SUCCEEDED(romfs_result);
    const bool romfs_base_only = romfs_mounted && g.romfs_base_only;
    g.romfs_base_only = was_base_only || romfs_base_only;

    const NsApplicationContentMetaStatus *selected = update ? update : base;
    if (romfs_base_only && update && base) {
        log_line("exefs-warning base-only: the update RomFS is unavailable; using the base ExeFS to match");
        selected = base;
        g.exefs_base_only = true;
    }

    FsFileSystem exefs = {0};
    Result exefs_result = open_title_exefs(
        &exefs, application_id, selected,
        selected == update ? base : NULL);
    if (R_FAILED(exefs_result) && selected == update && base) {
        log_line("exefs-warning base-only: the update ExeFS could not be opened (0x%08x); using the base ExeFS",
                 exefs_result);
        exefs_result = open_title_exefs(&exefs, application_id, base, NULL);
        if (R_SUCCEEDED(exefs_result)) g.exefs_base_only = true;
        if (R_SUCCEEDED(exefs_result) && romfs_mounted) {
            /* The mounted RomFS includes the update; with the base ExeFS
               the dump would mix two game versions, so use the base RomFS
               too. */
            log_line("romfs-warning base-only: the update ExeFS is unavailable; using the base RomFS to match");
            unmount_title_romfs("kdrom", romfs_device);
            romfs_device = false;
            romfs_result = mount_base_title_romfs(application_id,
                                                  &romfs_device);
            romfs_mounted = R_SUCCEEDED(romfs_result);
            g.romfs_base_only = true;
        }
    }
    if (R_SUCCEEDED(exefs_result)) {
        snprintf(destination, sizeof(destination), "%s/exefs", package);
        exefs_result = copy_filesystem_tree(
            &exefs, "/", destination, "exefs", pad);
        close_code_filesystem(&exefs);
    }

    bool cancelled = R_MODULE(exefs_result) == Module_Libnx &&
                     R_DESCRIPTION(exefs_result) == KAIDUMP_ERROR_CANCELED;
    if (romfs_mounted) {
        if (cancelled) {
            romfs_result = exefs_result;
        } else {
            snprintf(destination, sizeof(destination), "%s/romfs", package);
            romfs_result = copy_tree(
                "kdrom:/", destination, "romfs", pad);
        }
        unmount_title_romfs("kdrom", romfs_device);
    } else if (cancelled) {
        romfs_result = exefs_result;
    }

    Result result = R_FAILED(exefs_result) ? exefs_result : romfs_result;
    log_line("combined-game-finish id=%016llX package=%s exefs=0x%08x romfs=0x%08x result=0x%08x",
             (unsigned long long)application_id, package,
             exefs_result, romfs_result, result);
    return result;
}

Result export_combined_game(u64 application_id, const char *root,
                            PadState *pad) {
    char package[512];
    u8 programs[8] = {0};
    s32 program_count = game_program_indices(programs, 8);
    if (program_count <= 1) {
        snprintf(package, sizeof(package), "%s/game", root);
        return export_combined_program(application_id, package, pad);
    }

    /* Switch 2 Editions: the original game (program 0) goes in game/,
       every other program in game/program<N>/. */
    Result result = 0;
    for (s32 i = 0; i < program_count && R_SUCCEEDED(result); ++i) {
        if (programs[i])
            snprintf(package, sizeof(package), "%s/game/program%u", root,
                     programs[i]);
        else
            snprintf(package, sizeof(package), "%s/game", root);
        set_program_index_override(programs[i]);
        result = export_combined_program(application_id, package, pad);
        set_program_index_override(-1);
    }
    return result;
}
