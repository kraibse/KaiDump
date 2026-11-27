#include "nup.h"
#include "source_io.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <switch/crypto/sha256.h>

#define NUP_HEADER_SIZE 0x80
#define NUP_NO_DIR UINT32_MAX
#define NUP_ALIGN 16
#define NUP_CANCELED MAKERESULT(Module_Libnx, 200)

typedef struct {
    bool directory;
    uint32_t index;
    char *name;
} PlanChild;

typedef struct {
    char *name;
    PlanChild *children;
    size_t child_count;
    size_t child_capacity;
    uint32_t first_child;
    uint32_t name_offset;
} PlanDir;

typedef struct {
    char *name;
    char *path;
    uint64_t size;
    uint64_t extent_offset;
    uint64_t extent_size;
    uint8_t hash[32];
    uint32_t name_offset;
    uint32_t package_index;
    uint64_t source_offset;
    bool external;
} PlanFile;

typedef struct {
    uint64_t title_id;
    uint32_t root_dir;
    uint32_t exefs_dir;
    uint32_t romfs_dir;
    uint32_t version;
    uint32_t first_file;
    uint32_t file_count;
    uint8_t kind;
    uint8_t flags;
    uint8_t *meta;
    size_t meta_size;
    uint64_t meta_offset;
} PlanPackage;

typedef struct {
    NupInput *inputs;
    PlanPackage *packages;
    size_t package_count;
    PlanDir *dirs;
    size_t dir_count;
    size_t dir_capacity;
    PlanFile *files;
    size_t file_count;
    size_t file_capacity;
    uint64_t toc_size;
    uint64_t blob_offset;
    uint64_t blob_end;
    uint64_t file_size;
} Plan;

typedef struct {
    char *path;
    uint64_t size;
    uint32_t file_index;
    /* Files that only exist in the tree (control.nacp, icon): their bytes,
       owned by the plan's package meta. NULL for files from the sources. */
    const uint8_t *data;
    uint32_t parent;
    uint32_t *children;
    size_t child_count;
    size_t child_capacity;
    bool directory;
} TreeEntry;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t header_size;
    uint16_t header_version;
    uint64_t toc_offset;
    uint64_t toc_size;
    uint8_t toc_hash[32];
    uint64_t blob_offset;
    uint64_t file_size;
    uint32_t package_count;
    uint32_t flags;
    uint8_t uuid[16];
    uint8_t reserved[32];
} NupHeader;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint32_t package_count;
    uint64_t dir_count;
    uint64_t file_count;
    uint64_t string_pool_offset;
    uint64_t string_pool_size;
    uint64_t reserved;
} NupTocHeader;

typedef struct __attribute__((packed)) {
    uint64_t title_id;
    uint32_t root_dir;
    uint32_t exefs_dir;
    uint32_t romfs_dir;
    uint32_t version;
    uint8_t kind;
    uint8_t flags;
    uint16_t reserved;
    uint32_t reserved2;
    uint64_t meta_offset;
    uint32_t meta_size;
    uint8_t reserved3[20];
} NupPackageRecord;

typedef struct __attribute__((packed)) {
    uint32_t name_offset;
    uint32_t name_length;
    uint32_t first_child;
    uint32_t child_count;
    uint64_t reserved;
} NupDirRecord;

typedef struct __attribute__((packed)) {
    uint32_t name_offset;
    uint32_t name_length;
    uint64_t size;
    uint64_t extent_offset;
    uint64_t extent_size;
    uint8_t hash[32];
    uint8_t codec;
    uint8_t codec_flags;
    uint16_t reserved2;
    uint8_t reserved3[12];
} NupFileRecord;

_Static_assert(sizeof(NupHeader) == 0x80, "NUP header size");
_Static_assert(sizeof(NupTocHeader) == 0x30, "NUP TOC header size");
_Static_assert(sizeof(NupPackageRecord) == 0x40, "NUP package size");
_Static_assert(sizeof(NupDirRecord) == 0x18, "NUP directory size");
_Static_assert(sizeof(NupFileRecord) == 0x50, "NUP file size");

struct NupArchive {
    Plan plan;
    uint8_t *prefix;
    size_t prefix_size;
    NupMountCallback mount;
    NupUnmountCallback unmount;
    void *mount_user;
    int active_package;
    FILE *active_file;
    uint32_t active_file_index;
    uint64_t active_file_position;
    NupMount active_mount;
    TreeEntry *tree;
    size_t tree_count;
    size_t tree_capacity;
    uint64_t tree_total_size;
};

static Result io_error(void) {
    return MAKERESULT(Module_Libnx, LibnxError_IoError);
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static char *join_path(const char *parent, const char *name) {
    size_t parent_length = strlen(parent);
    size_t name_length = strlen(name);
    bool slash = parent_length && parent[parent_length - 1] != '/';
    char *path = malloc(parent_length + slash + name_length + 1);
    if (!path) return NULL;
    memcpy(path, parent, parent_length);
    if (slash) path[parent_length++] = '/';
    memcpy(path + parent_length, name, name_length + 1);
    return path;
}

static bool grow_array(void **array, size_t *capacity, size_t wanted,
                       size_t element_size) {
    if (*capacity >= wanted) return true;
    size_t next = *capacity ? *capacity * 2 : 32;
    while (next < wanted) next *= 2;
    void *grown = realloc(*array, next * element_size);
    if (!grown) return false;
    *array = grown;
    *capacity = next;
    return true;
}

static Result add_directory(Plan *plan, const char *name, uint32_t *out_index) {
    if (!grow_array((void **)&plan->dirs, &plan->dir_capacity,
                    plan->dir_count + 1, sizeof(*plan->dirs)))
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    uint32_t index = (uint32_t)plan->dir_count++;
    plan->dirs[index] = (PlanDir){.name = strdup(name)};
    if (!plan->dirs[index].name)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    *out_index = index;
    return 0;
}

static bool add_child(PlanDir *dir, bool directory, uint32_t index,
                      const char *name) {
    if (!grow_array((void **)&dir->children, &dir->child_capacity,
                    dir->child_count + 1, sizeof(*dir->children))) return false;
    PlanChild *child = &dir->children[dir->child_count++];
    child->directory = directory;
    child->index = index;
    child->name = strdup(name);
    return child->name != NULL;
}

static int child_compare(const void *left, const void *right) {
    const PlanChild *a = left;
    const PlanChild *b = right;
    return strcmp(a->name, b->name);
}

static Result scan_directory(Plan *plan, const char *path, const char *name,
                             uint32_t package_index,
                             NupCancelCallback cancelled, void *cancel_user,
                             uint32_t *out_index) {
    uint32_t dir_index = 0;
    Result result = add_directory(plan, name, &dir_index);
    if (R_FAILED(result)) return result;

    DIR *directory = opendir(path);
    if (!directory) return io_error();
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (cancelled && cancelled(cancel_user)) {
            result = NUP_CANCELED;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *child_path = join_path(path, entry->d_name);
        if (!child_path) {
            result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            break;
        }
        struct stat info;
        if (stat(child_path, &info) != 0) {
            free(child_path);
            result = io_error();
            break;
        }
        if (S_ISDIR(info.st_mode)) {
            uint32_t child_index = 0;
            result = scan_directory(plan, child_path, entry->d_name,
                                    package_index, cancelled, cancel_user,
                                    &child_index);
            if (R_SUCCEEDED(result) &&
                !add_child(&plan->dirs[dir_index], true, child_index,
                           entry->d_name))
                result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            free(child_path);
        } else if (S_ISREG(info.st_mode)) {
            if (!grow_array((void **)&plan->files, &plan->file_capacity,
                            plan->file_count + 1, sizeof(*plan->files))) {
                free(child_path);
                result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
                break;
            }
            uint32_t file_index = (uint32_t)plan->file_count++;
            plan->files[file_index] = (PlanFile){
                .name = strdup(entry->d_name),
                .path = child_path,
                .size = (uint64_t)info.st_size,
                .package_index = package_index,
            };
            if (!plan->files[file_index].name ||
                !add_child(&plan->dirs[dir_index], false, file_index,
                           entry->d_name))
                result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        } else {
            free(child_path);
        }
        if (R_FAILED(result)) break;
    }
    closedir(directory);
    if (R_SUCCEEDED(result) && plan->dirs[dir_index].child_count > 1)
        qsort(plan->dirs[dir_index].children,
              plan->dirs[dir_index].child_count,
              sizeof(*plan->dirs[dir_index].children), child_compare);
    if (R_SUCCEEDED(result)) *out_index = dir_index;
    return result;
}

static Result scan_external_exefs(Plan *plan, const NupMount *view,
                                  uint32_t package_index,
                                  uint32_t *out_index) {
    uint32_t dir_index = 0;
    Result result = add_directory(plan, "exefs", &dir_index);
    if (R_FAILED(result)) return result;

    for (size_t i = 0; i < view->exefs_file_count; ++i) {
        const NupExternalFile *source = &view->exefs_files[i];
        if (!source->name || !source->name[0] || strchr(source->name, '/') ||
            strchr(source->name, '\\') || !strcmp(source->name, ".") ||
            !strcmp(source->name, ".."))
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        if (!grow_array((void **)&plan->files, &plan->file_capacity,
                        plan->file_count + 1, sizeof(*plan->files)))
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        uint32_t file_index = (uint32_t)plan->file_count++;
        plan->files[file_index] = (PlanFile){
            .name = strdup(source->name),
            .size = source->size,
            .package_index = package_index,
            .source_offset = source->source_offset,
            .external = true,
        };
        if (!plan->files[file_index].name ||
            !add_child(&plan->dirs[dir_index], false, file_index,
                       source->name))
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    if (plan->dirs[dir_index].child_count > 1)
        qsort(plan->dirs[dir_index].children,
              plan->dirs[dir_index].child_count,
              sizeof(*plan->dirs[dir_index].children), child_compare);
    *out_index = dir_index;
    return 0;
}

/* Package meta blocks (SPEC §5) ------------------------------------------
 * A TLV list per package, stored after the blob region like the reference
 * packer does. Records are sorted by type; for a type that appears twice the
 * package-level record (NPDM, build ID, provenance) wins. */
#define NUP_NPDM_MAX_SIZE (256 * 1024)
#define NUP_META_MAX_RECORDS 32

typedef struct {
    uint16_t type;
    const uint8_t *data;
    uint32_t size;
} MetaRecord;

/* Size of an ExeFS file of the mounted package, or false if it is absent. */
static bool exefs_file_size(const NupMount *view, const char *name,
                            uint64_t *size, uint64_t *source_offset) {
    if (view->exefs[0]) {
        char *path = join_path(view->exefs, name);
        if (!path) return false;
        struct stat st;
        bool found = stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
        free(path);
        if (found) *size = (uint64_t)st.st_size;
        return found;
    }
    for (size_t i = 0; i < view->exefs_file_count; ++i) {
        if (!view->exefs_files[i].name ||
            strcmp(view->exefs_files[i].name, name))
            continue;
        *size = view->exefs_files[i].size;
        *source_offset = view->exefs_files[i].source_offset;
        return true;
    }
    return false;
}

static bool read_exefs_file(const NupMount *view, const char *name,
                            void *buffer, size_t size) {
    uint64_t file_size = 0;
    uint64_t source_offset = 0;
    if (!exefs_file_size(view, name, &file_size, &source_offset) ||
        file_size < size)
        return false;
    if (view->exefs[0]) {
        char *path = join_path(view->exefs, name);
        if (!path) return false;
        FILE *file = fopen(path, "rb");
        free(path);
        if (!file) return false;
        bool ok = fread(buffer, 1, size, file) == size;
        fclose(file);
        return ok;
    }
    if (!view->read_exefs) return false;
    size_t got = 0;
    return R_SUCCEEDED(view->read_exefs(view->read_exefs_user, source_offset,
                                        buffer, size, &got)) && got == size;
}

static void add_meta_record(MetaRecord *records, size_t *count,
                            uint16_t type, const void *data, uint32_t size) {
    if (*count >= NUP_META_MAX_RECORDS) return;
    records[(*count)++] = (MetaRecord){type, data, size};
}

static Result build_package_meta(PlanPackage *package, const NupMount *view) {
    MetaRecord records[NUP_META_MAX_RECORDS];
    size_t count = 0;

    uint8_t *npdm = NULL;
    uint64_t npdm_size = 0;
    uint64_t unused_offset = 0;
    if (exefs_file_size(view, "main.npdm", &npdm_size, &unused_offset) &&
        npdm_size && npdm_size <= NUP_NPDM_MAX_SIZE) {
        npdm = malloc((size_t)npdm_size);
        if (npdm && read_exefs_file(view, "main.npdm", npdm, (size_t)npdm_size))
            add_meta_record(records, &count, NUP_TLV_NPDM, npdm,
                            (uint32_t)npdm_size);
    }

    /* Build ID = ModuleId, bytes 0x40..0x60 of main (NSO0 or NRO0). */
    uint8_t header[0x60];
    if (read_exefs_file(view, "main", header, sizeof(header)) &&
        (!memcmp(header, "NSO0", 4) || !memcmp(header + 0x10, "NRO0", 4)))
        add_meta_record(records, &count, NUP_TLV_BUILD_ID, header + 0x40, 0x20);

    static const char source_dump[] = "KaiDump";
    add_meta_record(records, &count, NUP_TLV_SOURCE_DUMP, source_dump,
                    sizeof(source_dump) - 1);

    const uint8_t *extra = view->meta;
    for (size_t at = 0; extra && at + 8 <= view->meta_size;) {
        uint16_t type = (uint16_t)(extra[at] | extra[at + 1] << 8);
        uint32_t size = (uint32_t)extra[at + 4] | (uint32_t)extra[at + 5] << 8 |
                        (uint32_t)extra[at + 6] << 16 |
                        (uint32_t)extra[at + 7] << 24;
        if (size > view->meta_size - at - 8) break;
        add_meta_record(records, &count, type, extra + at + 8, size);
        at += 8 + (size_t)size;
    }

    /* Stable insertion sort by type, then drop later duplicates. */
    for (size_t i = 1; i < count; ++i) {
        MetaRecord record = records[i];
        size_t j = i;
        while (j && records[j - 1].type > record.type) {
            records[j] = records[j - 1];
            --j;
        }
        records[j] = record;
    }
    size_t total = 0;
    for (size_t i = 0; i < count; ++i)
        if (!i || records[i].type != records[i - 1].type)
            total += 8 + records[i].size;

    Result result = 0;
    uint8_t *block = total ? malloc(total) : NULL;
    if (total && !block) {
        result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    } else {
        size_t at = 0;
        for (size_t i = 0; i < count; ++i) {
            if (i && records[i].type == records[i - 1].type) continue;
            const MetaRecord *record = &records[i];
            block[at + 0] = (uint8_t)record->type;
            block[at + 1] = (uint8_t)(record->type >> 8);
            block[at + 2] = block[at + 3] = 0;
            for (int b = 0; b < 4; ++b)
                block[at + 4 + b] = (uint8_t)(record->size >> (8 * b));
            memcpy(block + at + 8, record->data, record->size);
            at += 8 + record->size;
        }
        package->meta = block;
        package->meta_size = total;
    }
    free(npdm);
    return result;
}

/* Copies from the meta region (after the blob region) into output. */
static size_t read_meta_region(const Plan *plan, uint64_t offset,
                               uint8_t *output, size_t size) {
    for (size_t i = 0; i < plan->package_count; ++i) {
        const PlanPackage *package = &plan->packages[i];
        if (!package->meta_size || offset < package->meta_offset ||
            offset >= package->meta_offset + package->meta_size)
            continue;
        uint64_t within = offset - package->meta_offset;
        size_t amount = package->meta_size - within > size
            ? size : (size_t)(package->meta_size - within);
        memcpy(output, package->meta + within, amount);
        return amount;
    }
    memset(output, 0, size);
    return size;
}

bool nupMetaFind(const void *meta, size_t meta_size, uint16_t type,
                 const uint8_t **data, uint32_t *size) {
    const uint8_t *block = meta;
    size_t at = 0;
    while (block && at + 8 <= meta_size) {
        const uint16_t record_type = (uint16_t)(block[at] | block[at + 1] << 8);
        const uint32_t record_size = (uint32_t)block[at + 4] |
            (uint32_t)block[at + 5] << 8 | (uint32_t)block[at + 6] << 16 |
            (uint32_t)block[at + 7] << 24;
        if (record_size > meta_size - at - 8) break;
        if (record_type == type) {
            *data = block + at + 8;
            *size = record_size;
            return true;
        }
        at += 8 + (size_t)record_size;
    }
    return false;
}

/* NACP languages (icon_<Name>.dat in the control data) and the BCP-47
   tags NUP uses for them (SPEC Appendix B). */
static const struct {
    const char *name;
    const char *tag;
} g_nup_languages[] = {
    {"AmericanEnglish", "en-us"},    {"BritishEnglish", "en-gb"},
    {"Japanese", "ja"},              {"French", "fr"},
    {"German", "de"},                {"LatinAmericanSpanish", "es-419"},
    {"Spanish", "es"},               {"Italian", "it"},
    {"Dutch", "nl"},                 {"CanadianFrench", "fr-ca"},
    {"Portuguese", "pt"},            {"Russian", "ru"},
    {"Korean", "ko"},                {"TraditionalChinese", "zh-hant"},
    {"SimplifiedChinese", "zh-hans"}, {"BrazilianPortuguese", "pt-br"},
};
#define NUP_LANGUAGE_COUNT \
    (sizeof(g_nup_languages) / sizeof(g_nup_languages[0]))

const char *nupLanguageTag(const char *name) {
    for (size_t i = 0; i < NUP_LANGUAGE_COUNT; ++i)
        if (!strcmp(g_nup_languages[i].name, name))
            return g_nup_languages[i].tag;
    return NULL;
}

static const char *language_name(const uint8_t *tag, size_t length) {
    for (size_t i = 0; i < NUP_LANGUAGE_COUNT; ++i) {
        const char *known = g_nup_languages[i].tag;
        if (strlen(known) != length) continue;
        size_t at = 0;
        while (at < length) {
            char c = (char)tag[at];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != known[at]) break;
            ++at;
        }
        if (at == length) return g_nup_languages[i].name;
    }
    return NULL;
}

size_t nupControlFiles(const void *meta, size_t meta_size,
                       NupControlFile *files, size_t capacity) {
    size_t count = 0;
    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (count < capacity &&
        nupMetaFind(meta, meta_size, NUP_TLV_NACP, &data, &size)) {
        snprintf(files[count].name, sizeof(files[count].name),
                 "control.nacp");
        files[count].data = data;
        files[count++].size = size;
    }
    const size_t icons_start = count;
    if (nupMetaFind(meta, meta_size, NUP_TLV_LOCALIZED_ICONS, &data, &size)) {
        size_t at = 0;
        while (at < size && count < capacity) {
            const size_t tag_length = data[at];
            if (at + 1 + tag_length + 4 > size) break;
            const uint8_t *tag = data + at + 1;
            const uint8_t *length = tag + tag_length;
            const uint32_t icon_size = (uint32_t)length[0] |
                (uint32_t)length[1] << 8 | (uint32_t)length[2] << 16 |
                (uint32_t)length[3] << 24;
            const size_t icon_at = at + 1 + tag_length + 4;
            if (icon_size > size - icon_at) break;
            const char *name = language_name(tag, tag_length);
            if (name) {
                snprintf(files[count].name, sizeof(files[count].name),
                         "icon_%s.dat", name);
                files[count].data = data + icon_at;
                files[count++].size = icon_size;
            }
            at = icon_at + icon_size;
        }
    }
    if (count == icons_start && count < capacity &&
        nupMetaFind(meta, meta_size, NUP_TLV_ICON, &data, &size)) {
        snprintf(files[count].name, sizeof(files[count].name),
                 "icon_AmericanEnglish.dat");
        files[count].data = data;
        files[count++].size = size;
    }
    return count;
}

static void free_plan(Plan *plan) {
    for (size_t i = 0; i < plan->dir_count; ++i) {
        free(plan->dirs[i].name);
        for (size_t j = 0; j < plan->dirs[i].child_count; ++j)
            free(plan->dirs[i].children[j].name);
        free(plan->dirs[i].children);
    }
    for (size_t i = 0; i < plan->file_count; ++i) {
        free(plan->files[i].name);
        free(plan->files[i].path);
    }
    for (size_t i = 0; plan->packages && i < plan->package_count; ++i)
        free(plan->packages[i].meta);
    free(plan->inputs);
    free(plan->packages);
    free(plan->dirs);
    free(plan->files);
    memset(plan, 0, sizeof(*plan));
}

static Result scan_sources(Plan *plan, const NupInput *inputs,
                           size_t input_count, NupMountCallback mount,
                           NupUnmountCallback unmount, void *mount_user,
                           NupCancelCallback cancelled, void *cancel_user) {
    plan->inputs = calloc(input_count, sizeof(*plan->inputs));
    plan->packages = calloc(input_count, sizeof(*plan->packages));
    if (!plan->inputs || !plan->packages)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memcpy(plan->inputs, inputs, input_count * sizeof(*inputs));
    plan->package_count = input_count;

    Result result = 0;
    for (size_t i = 0; i < input_count; ++i) {
        if (cancelled && cancelled(cancel_user)) return NUP_CANCELED;
        uint32_t root = 0;
        result = add_directory(plan, "", &root);
        if (R_FAILED(result)) return result;
        PlanPackage *package = &plan->packages[i];
        *package = (PlanPackage){
            .title_id = inputs[i].title_id,
            .root_dir = root,
            .exefs_dir = NUP_NO_DIR,
            .romfs_dir = NUP_NO_DIR,
            .version = inputs[i].version,
            .first_file = (uint32_t)plan->file_count,
            .kind = inputs[i].kind,
            .flags = inputs[i].flags,
        };

        NupMount view = {0};
        result = mount(&plan->inputs[i], &view, mount_user);
        if (R_SUCCEEDED(result) && view.version_set)
            package->version = view.version;
        if (R_SUCCEEDED(result) && view.exefs[0]) {
            result = scan_directory(plan, view.exefs, "exefs", (uint32_t)i,
                                    cancelled, cancel_user,
                                    &package->exefs_dir);
            if (R_SUCCEEDED(result) &&
                !add_child(&plan->dirs[root], true, package->exefs_dir,
                           "exefs"))
                result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        } else if (R_SUCCEEDED(result) && view.exefs_file_count &&
                   view.read_exefs) {
            result = scan_external_exefs(plan, &view, (uint32_t)i,
                                         &package->exefs_dir);
            if (R_SUCCEEDED(result) &&
                !add_child(&plan->dirs[root], true, package->exefs_dir,
                           "exefs"))
                result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        if (R_SUCCEEDED(result) && view.romfs[0]) {
            result = scan_directory(plan, view.romfs, "romfs", (uint32_t)i,
                                    cancelled, cancel_user,
                                    &package->romfs_dir);
            if (R_SUCCEEDED(result) &&
                !add_child(&plan->dirs[root], true, package->romfs_dir,
                           "romfs"))
                result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        if (R_SUCCEEDED(result))
            result = build_package_meta(package, &view);
        unmount(mount_user);
        if (R_FAILED(result)) return result;
        if (!view.exefs[0] && !view.exefs_file_count && !view.romfs[0])
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        package->file_count = (uint32_t)plan->file_count - package->first_file;
        if (plan->dirs[root].child_count > 1)
            qsort(plan->dirs[root].children, plan->dirs[root].child_count,
                  sizeof(*plan->dirs[root].children), child_compare);
    }

    uint64_t child_count = 0;
    uint64_t string_size = 0;
    for (size_t i = 0; i < plan->dir_count; ++i) {
        child_count += plan->dirs[i].child_count;
        string_size += strlen(plan->dirs[i].name);
    }
    for (size_t i = 0; i < plan->file_count; ++i)
        string_size += strlen(plan->files[i].name);
    plan->toc_size = sizeof(NupTocHeader) +
        plan->package_count * sizeof(NupPackageRecord) +
        plan->dir_count * sizeof(NupDirRecord) +
        plan->file_count * sizeof(NupFileRecord) +
        child_count * sizeof(uint32_t) + string_size;
    plan->blob_offset = align_up(NUP_HEADER_SIZE + plan->toc_size, NUP_ALIGN);

    uint64_t position = plan->blob_offset;
    for (size_t i = 0; i < plan->file_count; ++i) {
        PlanFile *file = &plan->files[i];
        if (file->size) position = align_up(position, NUP_ALIGN);
        file->extent_offset = position;
        file->extent_size = file->size;
        position += file->size;
    }
    plan->blob_end = position;
    for (size_t i = 0; i < plan->package_count; ++i) {
        PlanPackage *package = &plan->packages[i];
        if (!package->meta_size) continue;
        package->meta_offset = position;
        position += package->meta_size;
    }
    plan->file_size = position;
    return 0;
}

static int tree_compare(const void *left, const void *right) {
    const TreeEntry *a = left;
    const TreeEntry *b = right;
    return strcmp(a->path, b->path);
}

static Result tree_add(NupArchive *archive, const char *path, bool directory,
                       uint64_t size, uint32_t file_index) {
    if (!grow_array((void **)&archive->tree, &archive->tree_capacity,
                    archive->tree_count + 1, sizeof(*archive->tree)))
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    char *copy = strdup(path);
    if (!copy) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    archive->tree[archive->tree_count++] = (TreeEntry){
        .path = copy,
        .size = size,
        .file_index = file_index,
        .parent = UINT32_MAX,
        .directory = directory,
    };
    if (!directory) archive->tree_total_size += size;
    return 0;
}

static Result tree_add_memory(NupArchive *archive, const char *path,
                              const uint8_t *data, uint32_t size) {
    Result result = tree_add(archive, path, false, size, UINT32_MAX);
    if (R_SUCCEEDED(result))
        archive->tree[archive->tree_count - 1].data = data;
    return result;
}

/* Emulators read an extracted game's name, version and icons from
   control.nacp and icon_<Language>.dat next to its exefs/ and romfs/. */
static Result tree_add_control_files(NupArchive *archive,
                                     const PlanPackage *package,
                                     const char *prefix) {
    NupControlFile files[NUP_MAX_CONTROL_FILES];
    size_t count = nupControlFiles(package->meta, package->meta_size, files,
                                   NUP_MAX_CONTROL_FILES);
    Result result = 0;
    for (size_t i = 0; R_SUCCEEDED(result) && i < count; ++i) {
        char path[FS_MAX_PATH];
        const int length = snprintf(path, sizeof(path), "%s/%s", prefix,
                                    files[i].name);
        if (length < 0 || (size_t)length >= sizeof(path))
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        result = tree_add_memory(archive, path, files[i].data, files[i].size);
    }
    return result;
}

static char *tree_join(const char *parent, const char *name) {
    size_t parent_size = strlen(parent);
    size_t name_size = strlen(name);
    size_t total = parent_size + (parent_size ? 1 : 0) + name_size + 1;
    if (total > FS_MAX_PATH) return NULL;
    char *path = malloc(total);
    if (!path) return NULL;
    memcpy(path, parent, parent_size);
    if (parent_size) path[parent_size++] = '/';
    memcpy(path + parent_size, name, name_size + 1);
    return path;
}

static Result tree_add_parents(NupArchive *archive, const char *path) {
    char work[FS_MAX_PATH];
    if (snprintf(work, sizeof(work), "%s", path) >= (int)sizeof(work))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    for (char *at = work; *at; ++at) {
        if (*at != '/') continue;
        *at = 0;
        Result result = tree_add(archive, work, true, 0, UINT32_MAX);
        *at = '/';
        if (R_FAILED(result)) return result;
    }
    return tree_add(archive, work, true, 0, UINT32_MAX);
}

static Result tree_walk_directory(NupArchive *archive, uint32_t dir_index,
                                  const char *parent) {
    PlanDir *directory = &archive->plan.dirs[dir_index];
    for (size_t i = 0; i < directory->child_count; ++i) {
        PlanChild *child = &directory->children[i];
        char *path = tree_join(parent, child->name);
        if (!path) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        Result result;
        if (child->directory) {
            result = tree_add(archive, path, true, 0, UINT32_MAX);
            if (R_SUCCEEDED(result))
                result = tree_walk_directory(archive, child->index, path);
        } else {
            PlanFile *file = &archive->plan.files[child->index];
            result = tree_add(archive, path, false, file->size, child->index);
        }
        free(path);
        if (R_FAILED(result)) return result;
    }
    return 0;
}

static const char *tree_path(const TreeEntry *entry) {
    return entry ? entry->path : "";
}

static size_t tree_find_index(const NupArchive *archive, const char *path) {
    size_t low = 0;
    size_t high = archive->tree_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(archive->tree[middle].path, path);
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return low < archive->tree_count &&
           !strcmp(archive->tree[low].path, path) ? low : SIZE_MAX;
}

static Result build_tree(NupArchive *archive) {
    Result result = tree_add(archive, "", true, 0, UINT32_MAX);
    for (size_t i = 0; R_SUCCEEDED(result) && i < archive->plan.package_count;
         ++i) {
        const NupInput *input = &archive->plan.inputs[i];
        char prefix[128];
        uint8_t layout = input->layout;
        if (layout == NUP_LAYOUT_UNSPECIFIED) {
            layout = input->kind == NUP_KIND_UPDATE ? NUP_LAYOUT_UPDATE
                : input->kind == NUP_KIND_ADDON ||
                  input->kind == NUP_KIND_ADDON_UPDATE ? NUP_LAYOUT_DLC
                : NUP_LAYOUT_BASE;
        }
        if (layout == NUP_LAYOUT_GAME)
            snprintf(prefix, sizeof(prefix), "game");
        else if (layout == NUP_LAYOUT_GAME_PROGRAM)
            snprintf(prefix, sizeof(prefix), "game/program%u",
                     input->program_index);
        else if (layout == NUP_LAYOUT_BASE)
            snprintf(prefix, sizeof(prefix), "base");
        else if (layout == NUP_LAYOUT_BASE_PROGRAM)
            snprintf(prefix, sizeof(prefix), "base/program%u",
                     input->program_index);
        else if (layout == NUP_LAYOUT_UPDATE)
            snprintf(prefix, sizeof(prefix), "updates/%u", input->version);
        else
            snprintf(prefix, sizeof(prefix), "dlc/%016llX/0",
                     (unsigned long long)input->title_id);
        result = tree_add_parents(archive, prefix);
        if (R_SUCCEEDED(result))
            result = tree_walk_directory(
                archive, archive->plan.packages[i].root_dir, prefix);
        if (R_SUCCEEDED(result) && (layout == NUP_LAYOUT_GAME ||
                                    layout == NUP_LAYOUT_GAME_PROGRAM ||
                                    layout == NUP_LAYOUT_BASE ||
                                    layout == NUP_LAYOUT_BASE_PROGRAM))
            result = tree_add_control_files(
                archive, &archive->plan.packages[i], prefix);
    }
    if (R_FAILED(result)) return result;

    qsort(archive->tree, archive->tree_count, sizeof(*archive->tree),
          tree_compare);
    for (size_t i = 1; i < archive->tree_count; ++i) {
        if (strcmp(archive->tree[i - 1].path, archive->tree[i].path)) continue;
        if (!archive->tree[i - 1].directory || !archive->tree[i].directory)
            return MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    size_t kept = 0;
    for (size_t i = 0; i < archive->tree_count; ++i) {
        if (kept && !strcmp(archive->tree[kept - 1].path,
                            archive->tree[i].path)) {
            free(archive->tree[i].path);
            continue;
        }
        if (kept != i) archive->tree[kept] = archive->tree[i];
        ++kept;
    }
    archive->tree_count = kept;

    for (size_t i = 1; i < archive->tree_count; ++i) {
        char parent[FS_MAX_PATH];
        if (snprintf(parent, sizeof(parent), "%s", tree_path(&archive->tree[i]))
                >= (int)sizeof(parent))
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        else parent[0] = 0;
        size_t parent_index = tree_find_index(archive, parent);
        if (parent_index == SIZE_MAX ||
            !archive->tree[parent_index].directory)
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        archive->tree[i].parent = (uint32_t)parent_index;
        TreeEntry *owner = &archive->tree[parent_index];
        if (!grow_array((void **)&owner->children, &owner->child_capacity,
                        owner->child_count + 1, sizeof(*owner->children)))
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        owner->children[owner->child_count++] = (uint32_t)i;
    }
    return 0;
}

static Result write_zeros(FILE *output, uint64_t count) {
    static const uint8_t zeros[256] = {0};
    while (count) {
        size_t chunk = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count;
        if (fwrite(zeros, 1, chunk, output) != chunk) return io_error();
        count -= chunk;
    }
    return 0;
}

static Result read_and_hash_file(PlanFile *file, const NupMount *view,
                                 FILE *output, void *buffer,
                                 size_t buffer_size,
                                 NupCancelCallback cancelled, void *user,
                                 uint64_t *byte_count) {
    FILE *input = NULL;
    if (!file->external) {
        input = fopen(file->path, "rb");
        if (!input) return io_error();
        setvbuf(input, NULL, _IONBF, 0);
    } else if (!view->read_exefs) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    Sha256Context hash;
    sha256ContextCreate(&hash);
    uint64_t remaining = file->size;
    Result result = 0;
    while (remaining) {
        size_t wanted = remaining > buffer_size ? buffer_size : (size_t)remaining;
        size_t got = 0;
        Result read_result = 0;
        if (file->external) {
            read_result = view->read_exefs(
                view->read_exefs_user,
                file->source_offset + file->size - remaining,
                buffer, wanted, &got);
        } else {
            read_result = sourceReadExact(
                input, file->path, file->size - remaining, buffer, wanted);
            if (R_SUCCEEDED(read_result)) got = wanted;
        }
        if (R_FAILED(read_result) || got != wanted ||
            (output && fwrite(buffer, 1, got, output) != got)) {
            result = R_FAILED(read_result) ? read_result : io_error();
            break;
        }
        sha256ContextUpdate(&hash, buffer, got);
        remaining -= got;
        if (byte_count) *byte_count += got;
        if (cancelled && cancelled(user)) {
            result = NUP_CANCELED;
            break;
        }
    }
    if (input) fclose(input);
    if (R_SUCCEEDED(result)) sha256ContextGetHash(&hash, file->hash);
    return result;
}

static Result process_files(Plan *plan, FILE *output,
                            NupMountCallback mount,
                            NupUnmountCallback unmount, void *mount_user,
                            void *buffer, size_t buffer_size,
                            NupCancelCallback cancelled, void *cancel_user,
                            uint64_t *byte_count, uint64_t *file_count) {
    Result result = 0;
    uint64_t position = plan->blob_offset;
    for (size_t package_index = 0;
         R_SUCCEEDED(result) && package_index < plan->package_count;
         ++package_index) {
        NupMount view = {0};
        result = mount(&plan->inputs[package_index], &view, mount_user);
        if (R_FAILED(result)) break;
        PlanPackage *package = &plan->packages[package_index];
        uint32_t end = package->first_file + package->file_count;
        for (uint32_t i = package->first_file; R_SUCCEEDED(result) && i < end;
             ++i) {
            PlanFile *file = &plan->files[i];
            if (output && file->size) {
                result = write_zeros(output, file->extent_offset - position);
                position = file->extent_offset;
            }
            if (R_SUCCEEDED(result))
                result = read_and_hash_file(file, &view, output,
                                            buffer, buffer_size,
                                            cancelled, cancel_user, byte_count);
            if (R_SUCCEEDED(result)) {
                position += file->size;
                if (file_count) ++*file_count;
            }
        }
        unmount(mount_user);
    }
    return result;
}

static uint64_t plan_content_size(const Plan *plan) {
    uint64_t total = 0;
    for (size_t i = 0; i < plan->file_count; ++i) total += plan->files[i].size;
    return total;
}

static Result build_prefix(Plan *plan, uint8_t **out_prefix) {
    if (plan->blob_offset > SIZE_MAX)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    uint8_t *prefix = calloc(1, (size_t)plan->blob_offset);
    if (!prefix) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    uint8_t *toc = prefix + NUP_HEADER_SIZE;
    NupTocHeader *toc_header = (NupTocHeader *)toc;
    memcpy(toc_header->magic, "TOC\1", 4);
    toc_header->package_count = (uint32_t)plan->package_count;
    toc_header->dir_count = plan->dir_count;
    toc_header->file_count = plan->file_count;

    uint64_t child_count = 0;
    uint64_t string_size = 0;
    for (size_t i = 0; i < plan->dir_count; ++i) {
        child_count += plan->dirs[i].child_count;
        string_size += strlen(plan->dirs[i].name);
    }
    for (size_t i = 0; i < plan->file_count; ++i)
        string_size += strlen(plan->files[i].name);

    size_t packages_at = sizeof(*toc_header);
    size_t dirs_at = packages_at + plan->package_count * sizeof(NupPackageRecord);
    size_t files_at = dirs_at + plan->dir_count * sizeof(NupDirRecord);
    size_t children_at = files_at + plan->file_count * sizeof(NupFileRecord);
    size_t strings_at = children_at + (size_t)child_count * sizeof(uint32_t);
    toc_header->string_pool_offset = strings_at;
    toc_header->string_pool_size = string_size;

    NupPackageRecord *packages = (NupPackageRecord *)(toc + packages_at);
    for (size_t i = 0; i < plan->package_count; ++i) {
        PlanPackage *source = &plan->packages[i];
        packages[i] = (NupPackageRecord){
            .title_id = source->title_id,
            .root_dir = source->root_dir,
            .exefs_dir = source->exefs_dir,
            .romfs_dir = source->romfs_dir,
            .version = source->version,
            .kind = source->kind,
            .flags = source->flags,
            .meta_offset = source->meta_offset,
            .meta_size = (uint32_t)source->meta_size,
        };
    }

    NupDirRecord *dirs = (NupDirRecord *)(toc + dirs_at);
    NupFileRecord *files = (NupFileRecord *)(toc + files_at);
    uint32_t *children = (uint32_t *)(toc + children_at);
    uint8_t *strings = toc + strings_at;
    uint32_t child_at = 0;
    uint32_t string_at = 0;
    for (size_t i = 0; i < plan->dir_count; ++i) {
        PlanDir *source = &plan->dirs[i];
        size_t length = strlen(source->name);
        source->first_child = child_at;
        source->name_offset = string_at;
        memcpy(strings + string_at, source->name, length);
        string_at += (uint32_t)length;
        dirs[i] = (NupDirRecord){
            .name_offset = source->name_offset,
            .name_length = (uint32_t)length,
            .first_child = source->first_child,
            .child_count = (uint32_t)source->child_count,
        };
        for (size_t j = 0; j < source->child_count; ++j) {
            PlanChild *child = &source->children[j];
            children[child_at++] = child->directory
                ? child->index : (uint32_t)plan->dir_count + child->index;
        }
    }
    for (size_t i = 0; i < plan->file_count; ++i) {
        PlanFile *source = &plan->files[i];
        size_t length = strlen(source->name);
        source->name_offset = string_at;
        memcpy(strings + string_at, source->name, length);
        string_at += (uint32_t)length;
        files[i] = (NupFileRecord){
            .name_offset = source->name_offset,
            .name_length = (uint32_t)length,
            .size = source->size,
            /* SPEC §3.6: zero-length files have no extent. The plan keeps
               their position so find_file can binary-search by offset. */
            .extent_offset = source->size ? source->extent_offset : 0,
            .extent_size = source->extent_size,
            .codec = 0,
        };
        memcpy(files[i].hash, source->hash, sizeof(source->hash));
    }

    NupHeader *header = (NupHeader *)prefix;
    memcpy(header->magic, "NUP\1", 4);
    header->header_size = NUP_HEADER_SIZE;
    header->header_version = 1;
    header->toc_offset = NUP_HEADER_SIZE;
    header->toc_size = plan->toc_size;
    header->blob_offset = plan->blob_offset;
    header->file_size = plan->file_size;
    header->package_count = (uint32_t)plan->package_count;
    sha256CalculateHash(header->toc_hash, toc, (size_t)plan->toc_size);
    Sha256Context uuid_hash;
    sha256ContextCreate(&uuid_hash);
    for (size_t i = 0; i < plan->package_count; ++i) {
        sha256ContextUpdate(&uuid_hash, &plan->packages[i].title_id, 8);
        sha256ContextUpdate(&uuid_hash, &plan->packages[i].version, 4);
        sha256ContextUpdate(&uuid_hash, &plan->packages[i].kind, 1);
    }
    uint8_t uuid[32];
    sha256ContextGetHash(&uuid_hash, uuid);
    memcpy(header->uuid, uuid, sizeof(header->uuid));
    *out_prefix = prefix;
    return 0;
}

Result nupCreateDirect(const char *output_path, const NupInput *inputs,
                       size_t input_count, NupMountCallback mount,
                       NupUnmountCallback unmount, void *mount_user,
                       void *copy_buffer, size_t buffer_size,
                       NupCancelCallback cancelled, void *cancel_user,
                       uint64_t *total_bytes, uint64_t *written_bytes,
                       uint64_t *packed_files) {
    if (!output_path || !inputs || !input_count || !mount || !unmount ||
        !copy_buffer || !buffer_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (total_bytes) *total_bytes = 0;
    if (written_bytes) *written_bytes = 0;
    if (packed_files) *packed_files = 0;

    Plan plan = {0};
    Result result = scan_sources(&plan, inputs, input_count, mount, unmount,
                                 mount_user, cancelled, cancel_user);
    if (R_SUCCEEDED(result) && total_bytes)
        *total_bytes = plan_content_size(&plan);
    FILE *output = NULL;
    void *output_buffer = NULL;
    if (R_SUCCEEDED(result)) {
        output = fopen(output_path, "wb+");
        if (!output) result = io_error();
    }
    if (output) {
        output_buffer = malloc(1024 * 1024);
        if (output_buffer &&
            setvbuf(output, output_buffer, _IOFBF, 1024 * 1024) != 0) {
            free(output_buffer);
            output_buffer = NULL;
        }
    }
    if (R_SUCCEEDED(result) &&
        ftruncate(fileno(output), (off_t)plan.file_size) != 0)
        result = io_error();
    if (R_SUCCEEDED(result)) result = write_zeros(output, plan.blob_offset);
    if (R_SUCCEEDED(result))
        result = process_files(&plan, output, mount, unmount, mount_user,
                               copy_buffer, buffer_size, cancelled, cancel_user,
                               written_bytes, packed_files);
    for (size_t i = 0; R_SUCCEEDED(result) && i < plan.package_count; ++i) {
        const PlanPackage *package = &plan.packages[i];
        if (!package->meta_size) continue;
        if (fseeko(output, (off_t)package->meta_offset, SEEK_SET) != 0 ||
            fwrite(package->meta, 1, package->meta_size, output) !=
                package->meta_size)
            result = io_error();
    }
    uint8_t *prefix = NULL;
    if (R_SUCCEEDED(result)) result = build_prefix(&plan, &prefix);
    if (R_SUCCEEDED(result) &&
        (fseeko(output, 0, SEEK_SET) != 0 ||
         fwrite(prefix, 1, (size_t)plan.blob_offset, output) != plan.blob_offset))
        result = io_error();
    free(prefix);
    if (output && R_FAILED(result)) {
        off_t partial_size = ftello(output);
        fflush(output);
        if (partial_size >= 0) ftruncate(fileno(output), partial_size);
    }
    if (output && fclose(output) != 0 && R_SUCCEEDED(result)) result = io_error();
    free(output_buffer);
    free_plan(&plan);
    return result;
}

Result nupPrepareVirtual(NupArchive **out_archive, const NupInput *inputs,
                         size_t input_count, NupMountCallback mount,
                         NupUnmountCallback unmount, void *mount_user,
                         void *read_buffer, size_t buffer_size,
                         NupCancelCallback cancelled, void *cancel_user,
                         uint64_t *total_bytes, uint64_t *hashed_bytes,
                         uint64_t *packed_files) {
    if (!out_archive || !inputs || !input_count || !mount || !unmount ||
        !read_buffer || !buffer_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_archive = NULL;
    if (total_bytes) *total_bytes = 0;
    if (hashed_bytes) *hashed_bytes = 0;
    if (packed_files) *packed_files = 0;
    NupArchive *archive = calloc(1, sizeof(*archive));
    if (!archive) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    archive->mount = mount;
    archive->unmount = unmount;
    archive->mount_user = mount_user;
    archive->active_package = -1;
    archive->active_file_index = UINT32_MAX;

    Result result = scan_sources(&archive->plan, inputs, input_count,
                                 mount, unmount, mount_user,
                                 cancelled, cancel_user);
    if (R_SUCCEEDED(result) && total_bytes)
        *total_bytes = plan_content_size(&archive->plan);
    if (R_SUCCEEDED(result))
        result = process_files(&archive->plan, NULL, mount, unmount, mount_user,
                               read_buffer, buffer_size, cancelled, cancel_user,
                               hashed_bytes, packed_files);
    if (R_SUCCEEDED(result)) result = build_prefix(&archive->plan, &archive->prefix);
    if (R_SUCCEEDED(result)) archive->prefix_size = archive->plan.blob_offset;
    if (R_FAILED(result)) {
        nupArchiveClose(archive);
        return result;
    }
    *out_archive = archive;
    return 0;
}

Result nupPrepareTree(NupArchive **out_archive, const NupInput *inputs,
                      size_t input_count, NupMountCallback mount,
                      NupUnmountCallback unmount, void *mount_user,
                      NupCancelCallback cancelled, void *cancel_user,
                      uint64_t *file_count) {
    if (!out_archive || !inputs || !input_count || !mount || !unmount)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_archive = NULL;
    if (file_count) *file_count = 0;

    NupArchive *archive = calloc(1, sizeof(*archive));
    if (!archive) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    archive->mount = mount;
    archive->unmount = unmount;
    archive->mount_user = mount_user;
    archive->active_package = -1;
    archive->active_file_index = UINT32_MAX;

    Result result = scan_sources(&archive->plan, inputs, input_count,
                                 mount, unmount, mount_user,
                                 cancelled, cancel_user);
    if (R_SUCCEEDED(result)) result = build_tree(archive);
    if (R_FAILED(result)) {
        nupArchiveClose(archive);
        return result;
    }
    if (file_count) *file_count = archive->plan.file_count;
    *out_archive = archive;
    return 0;
}

uint64_t nupArchiveSize(const NupArchive *archive) {
    return archive ? archive->plan.file_size : 0;
}

static size_t find_file(const Plan *plan, uint64_t offset) {
    size_t low = 0;
    size_t high = plan->file_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const PlanFile *file = &plan->files[middle];
        if (file->extent_offset <= offset) low = middle + 1;
        else high = middle;
    }
    size_t upper = low;
    while (low) {
        const PlanFile *file = &plan->files[--low];
        if (file->extent_size &&
            offset < file->extent_offset + file->extent_size)
            return low;
        if (file->extent_offset < offset && file->extent_size) break;
    }
    while (upper < plan->file_count && !plan->files[upper].extent_size)
        ++upper;
    return upper;
}

static Result activate_archive_package(NupArchive *archive,
                                       uint32_t package_index) {
    if (archive->active_package == (int)package_index) return 0;
    if (archive->active_file) {
        fclose(archive->active_file);
        archive->active_file = NULL;
        archive->active_file_index = UINT32_MAX;
        archive->active_file_position = 0;
    }
    if (archive->active_package >= 0) archive->unmount(archive->mount_user);
    archive->active_package = -1;
    memset(&archive->active_mount, 0, sizeof(archive->active_mount));
    Result result = archive->mount(&archive->plan.inputs[package_index],
                                   &archive->active_mount,
                                   archive->mount_user);
    if (R_SUCCEEDED(result)) archive->active_package = (int)package_index;
    return result;
}

static Result read_plan_file(NupArchive *archive, uint32_t index,
                             uint64_t offset, void *buffer, size_t size,
                             size_t *bytes_read) {
    if (index >= archive->plan.file_count)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    PlanFile *file = &archive->plan.files[index];
    *bytes_read = 0;
    if (offset >= file->size || !size) return 0;
    if ((uint64_t)size > file->size - offset)
        size = (size_t)(file->size - offset);

    Result result = activate_archive_package(archive, file->package_index);
    if (R_FAILED(result)) return result;
    if (file->external) {
        if (archive->active_file) {
            fclose(archive->active_file);
            archive->active_file = NULL;
            archive->active_file_index = UINT32_MAX;
            archive->active_file_position = 0;
        }
        if (!archive->active_mount.read_exefs)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        result = archive->active_mount.read_exefs(
            archive->active_mount.read_exefs_user,
            file->source_offset + offset, buffer, size, bytes_read);
        if (R_SUCCEEDED(result) && *bytes_read != size) result = io_error();
        return result;
    }

    if (!archive->active_file || archive->active_file_index != index) {
        if (archive->active_file) fclose(archive->active_file);
        archive->active_file = fopen(file->path, "rb");
        if (!archive->active_file) return io_error();
        setvbuf(archive->active_file, NULL, _IONBF, 0);
        archive->active_file_index = index;
        archive->active_file_position = 0;
    }
    result = sourceReadExact(archive->active_file, file->path, offset,
                             buffer, size);
    if (R_FAILED(result)) return result;
    archive->active_file_position = offset + size;
    *bytes_read = size;
    return 0;
}

Result nupArchiveRead(NupArchive *archive, uint64_t offset, void *buffer,
                      size_t size, size_t *bytes_read) {
    if (!archive || !buffer || !bytes_read)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *bytes_read = 0;
    if (offset >= archive->plan.file_size || !size) return 0;
    if ((uint64_t)size > archive->plan.file_size - offset)
        size = (size_t)(archive->plan.file_size - offset);
    uint8_t *output = buffer;

    while (size) {
        if (offset < archive->prefix_size) {
            size_t amount = archive->prefix_size - offset > size
                ? size : (size_t)(archive->prefix_size - offset);
            memcpy(output, archive->prefix + offset, amount);
            output += amount;
            offset += amount;
            size -= amount;
            *bytes_read += amount;
            continue;
        }

        if (offset >= archive->plan.blob_end) {
            size_t amount = read_meta_region(&archive->plan, offset, output,
                                             size);
            output += amount;
            offset += amount;
            size -= amount;
            *bytes_read += amount;
            continue;
        }

        size_t index = find_file(&archive->plan, offset);
        if (index >= archive->plan.file_count) {
            /* Alignment padding before the meta region. */
            size_t amount = archive->plan.blob_end - offset > size
                ? size : (size_t)(archive->plan.blob_end - offset);
            memset(output, 0, amount);
            output += amount;
            offset += amount;
            size -= amount;
            *bytes_read += amount;
            continue;
        }
        PlanFile *file = &archive->plan.files[index];
        if (offset < file->extent_offset) {
            size_t amount = file->extent_offset - offset > size
                ? size : (size_t)(file->extent_offset - offset);
            memset(output, 0, amount);
            output += amount;
            offset += amount;
            size -= amount;
            *bytes_read += amount;
            continue;
        }

        uint64_t within = offset - file->extent_offset;
        size_t amount = file->size - within > size
            ? size : (size_t)(file->size - within);
        size_t got = 0;
        Result result = read_plan_file(archive, (uint32_t)index, within,
                                       output, amount, &got);
        if (R_FAILED(result) || got != amount)
            return R_FAILED(result) ? result : io_error();
        output += amount;
        offset += amount;
        size -= amount;
        *bytes_read += amount;
    }
    return 0;
}

static bool normalize_tree_path(const char *path, char *normalized,
                                size_t normalized_size) {
    if (!path || !normalized || !normalized_size) return false;
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    while (*path == '/') ++path;
    size_t length = strlen(path);
    while (length && path[length - 1] == '/') --length;
    if (length >= normalized_size) return false;
    memcpy(normalized, path, length);
    normalized[length] = 0;
    return strstr(normalized, "//") == NULL;
}

static void fill_tree_entry(const TreeEntry *source, NupTreeEntry *entry) {
    const char *name = strrchr(source->path, '/');
    name = name ? name + 1 : source->path;
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->size = source->size;
    entry->directory = source->directory;
}

bool nupTreeStat(NupArchive *archive, const char *path, NupTreeEntry *entry) {
    if (!archive || !entry) return false;
    char normalized[FS_MAX_PATH];
    if (!normalize_tree_path(path, normalized, sizeof(normalized))) return false;
    size_t index = tree_find_index(archive, normalized);
    if (index == SIZE_MAX) return false;
    fill_tree_entry(&archive->tree[index], entry);
    return true;
}

bool nupTreeList(NupArchive *archive, const char *path, size_t index,
                 NupTreeEntry *entry) {
    if (!archive || !entry) return false;
    char normalized[FS_MAX_PATH];
    if (!normalize_tree_path(path, normalized, sizeof(normalized))) return false;
    size_t directory_index = tree_find_index(archive, normalized);
    if (directory_index == SIZE_MAX ||
        !archive->tree[directory_index].directory ||
        index >= archive->tree[directory_index].child_count)
        return false;
    uint32_t child = archive->tree[directory_index].children[index];
    fill_tree_entry(&archive->tree[child], entry);
    return true;
}

Result nupTreeRead(NupArchive *archive, const char *path, uint64_t offset,
                   void *buffer, size_t size, size_t *bytes_read) {
    if (!archive || !buffer || !bytes_read)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    char normalized[FS_MAX_PATH];
    if (!normalize_tree_path(path, normalized, sizeof(normalized)))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    size_t index = tree_find_index(archive, normalized);
    if (index == SIZE_MAX || archive->tree[index].directory)
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    const TreeEntry *entry = &archive->tree[index];
    if (entry->data) {
        *bytes_read = 0;
        if (offset >= entry->size || !size) return 0;
        if ((uint64_t)size > entry->size - offset)
            size = (size_t)(entry->size - offset);
        memcpy(buffer, entry->data + offset, size);
        *bytes_read = size;
        return 0;
    }
    return read_plan_file(archive, entry->file_index,
                          offset, buffer, size, bytes_read);
}

uint64_t nupTreeTotalSize(const NupArchive *archive) {
    return archive ? archive->tree_total_size : 0;
}

void nupArchiveDeactivate(NupArchive *archive) {
    if (!archive) return;
    if (archive->active_file) {
        fclose(archive->active_file);
        archive->active_file = NULL;
    }
    if (archive->active_package >= 0) {
        archive->unmount(archive->mount_user);
        archive->active_package = -1;
    }
    archive->active_file_index = UINT32_MAX;
    archive->active_file_position = 0;
    memset(&archive->active_mount, 0, sizeof(archive->active_mount));
}

void nupArchiveClose(NupArchive *archive) {
    if (!archive) return;
    nupArchiveDeactivate(archive);
    free(archive->prefix);
    for (size_t i = 0; i < archive->tree_count; ++i) {
        free(archive->tree[i].path);
        free(archive->tree[i].children);
    }
    free(archive->tree);
    free_plan(&archive->plan);
    free(archive);
}
