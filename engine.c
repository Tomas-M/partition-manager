#define _GNU_SOURCE
#include "partition-manager.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

Filesystem filesystems[]={
    {"ext4", "ext4", "mkfs.ext4", "resize2fs", FALSE, FALSE},
    {"xfs", "XFS", "mkfs.xfs", "xfs_growfs", FALSE, FALSE},
    {"btrfs", "Btrfs", "mkfs.btrfs", "btrfs", FALSE, FALSE},
    {"ntfs", "NTFS", "mkfs.ntfs", "ntfsresize", FALSE, FALSE},
    {"vfat", "FAT32", "mkfs.fat", NULL, FALSE, FALSE},
    {"exfat", "exFAT", "mkfs.exfat", NULL, FALSE, FALSE}
};
const guint filesystem_count=G_N_ELEMENTS(filesystems);
static char *lsblk_columns;

/* Run argv directly; a pinned disk is inherited as fd 3, never reopened by name. */
gboolean command_run(const char *const *argv, const char *input, int fd, char **output, char **diagnostic, GError **error)
{
    GSubprocessLauncher *launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE|G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
    if (fd>=0)
    {
        int inherited=dup(fd);
        if (inherited<0) { g_object_unref(launcher); return fail(error,"Cannot pass the pinned device to a tool: %s",g_strerror(errno)); }
        g_subprocess_launcher_take_fd(launcher,inherited,3);
    }
    GSubprocess *process=g_subprocess_launcher_spawnv(launcher, argv, error);
    g_object_unref(launcher);
    if (!process) return FALSE;
    char *out=NULL, *err=NULL;
    gboolean result=g_subprocess_communicate_utf8(process, input, NULL, &out, &err, error);
    if (result && !g_subprocess_get_successful(process))
    {
        int code=g_subprocess_get_if_exited(process)?g_subprocess_get_exit_status(process):128;
        g_set_error(error, G_SPAWN_EXIT_ERROR, code, "%s failed.\n%s%s", argv[0], err?err:"", out?out:"");
        result=FALSE;
    }
    if (output) *output=out; else g_free(out);
    if (diagnostic) *diagnostic=err; else g_free(err);
    g_object_unref(process);
    return result;
}

/* Optional inventory fields are null when the kernel or udev has no value. */
static const char *js(JsonObject *object, const char *key)
{
    JsonNode *node=json_object_get_member(object, key);
    return node && JSON_NODE_HOLDS_VALUE(node)?json_node_get_string(node):"";
}

/* util-linux uses both JSON numbers and numeric strings for byte counts. */
static guint64 jn(JsonObject *object, const char *key)
{
    JsonNode *node=json_object_get_member(object, key);
    if (!node || JSON_NODE_HOLDS_NULL(node)) return 0;
    if (json_node_get_value_type(node)==G_TYPE_BOOLEAN) return json_node_get_boolean(node);
    return json_node_get_value_type(node)==G_TYPE_STRING?g_ascii_strtoull(json_node_get_string(node), NULL, 10):(guint64)json_node_get_int(node);
}

/* Replace model text with UTF-8 from a command result. */
static void text_set(char **target, const char *value)
{
    g_free(*target); *target=g_utf8_make_valid(value?value:"", -1);
}

/* Parse one command result without keeping pointers into temporary JSON data. */
static JsonParser *parse_json(const char *text, GError **error)
{
    JsonParser *parser=json_parser_new();
    if (json_parser_load_from_data(parser, text, -1, error)) return parser;
    g_object_unref(parser);
    return NULL;
}

/* Discover tools and optional inventory columns once, outside GTK callbacks. */
gboolean engine_init(GError **error)
{
    const char *required[]={"sfdisk", "lsblk", "blkid", "wipefs", NULL};
    for (guint i=0; required[i]; i++)
    {
        char *path=g_find_program_in_path(required[i]);
        if (!path) return fail(error, "Required program not found: %s", required[i]);
        g_free(path);
    }
    for (guint i=0; i<filesystem_count; i++)
    {
        char *path=g_find_program_in_path(filesystems[i].formatter);
        filesystems[i].format_available=path!=NULL; g_free(path);
        path=filesystems[i].grower?g_find_program_in_path(filesystems[i].grower):NULL;
        filesystems[i].grow_available=path!=NULL; g_free(path);
    }
    const char *argv[]={"lsblk", "--list-columns", "--json", NULL};
    char *output=NULL;
    if (!command_run(argv, NULL, -1, &output, NULL, error)) return FALSE;
    JsonParser *parser=parse_json(output, error); g_free(output);
    if (!parser) return FALSE;
    JsonArray *array=json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "lsblk-columns");
    const char *wanted[]={"NAME", "PATH", "TYPE", "SIZE", "LOG-SEC", "MODEL", "TRAN", "SERIAL", "WWN", "MAJ:MIN", "RO", "FSTYPE", "UUID", "LABEL", "MOUNTPOINTS", "FSSIZE", "PKNAME", NULL};
    GString *columns=g_string_new(NULL);
    for (guint i=0; wanted[i]; i++)
        for (guint j=0; j<json_array_get_length(array); j++)
            if (!g_ascii_strcasecmp(js(json_array_get_object_element(array, j), "holder"), wanted[i]))
            {
                if (columns->len) g_string_append_c(columns, ',');
                g_string_append(columns, wanted[i]); break;
            }
    g_free(lsblk_columns); lsblk_columns=g_string_free(columns, FALSE);
    g_object_unref(parser);
    return TRUE;
}

/* Read explicit inventory columns, deduplicated by full device path. */
static JsonParser *inventory(const char *path, GError **error)
{
    const char *argv[]={"lsblk", "--json", "--list", "--bytes", "--paths", "--output", lsblk_columns, path, NULL};
    char *output=NULL;
    if (!command_run(argv, NULL, -1, &output, NULL, error)) return NULL;
    JsonParser *parser=parse_json(output, error); g_free(output);
    return parser;
}

/* Active device-mapper/RAID users count as busy even without a mount point. */
static gboolean has_holders(const char *majmin)
{
    char *path=g_build_filename("/sys/dev/block", majmin, "holders", NULL);
    GDir *dir=g_dir_open(path, 0, NULL); g_free(path);
    gboolean result=dir && g_dir_read_name(dir)!=NULL;
    if (dir) g_dir_close(dir);
    return result;
}

/* Mountpoint arrays include swap and all bind mounts, not only the last mount. */
static char *mounts(JsonObject *entry)
{
    GString *text=g_string_new(NULL);
    if (json_object_has_member(entry, "mountpoints"))
    {
        JsonArray *array=json_object_get_array_member(entry, "mountpoints");
        for (guint i=0; array && i<json_array_get_length(array); i++)
        {
            JsonNode *node=json_array_get_element(array, i);
            if (JSON_NODE_HOLDS_NULL(node)) continue;
            if (text->len) g_string_append_c(text, '\n');
            g_string_append(text, json_node_get_string(node));
        }
    }
    return g_string_free(text, FALSE);
}

/* Read the available type catalogue for the actual table, never cache GUIDs. */
static gboolean load_types(Disk *disk, GError **error)
{
    g_ptr_array_set_size(disk->types, 0);
    if (strcmp(disk->label, "gpt") && strcmp(disk->label, "dos")) return TRUE;
    const char *argv[]={"sfdisk", "--list-types", "--label", disk->label, NULL};
    char *output=NULL;
    if (!command_run(argv, NULL, -1, &output, NULL, error)) return FALSE;
    char **lines=g_strsplit(output, "\n", -1);
    for (char **line=lines; *line; line++)
    {
        char *id=g_strstrip(*line), *name=id+strcspn(id, " \t");
        if (!*name) continue;
        *name++=0; name=g_strstrip(name);
        gboolean guid=strlen(id)==36 && id[8]=='-';
        gboolean hex=strlen(id)<=2 && strspn(id, "0123456789abcdefABCDEF")==strlen(id);
        if (!guid && !hex) continue;
        PartitionType *type=g_new0(PartitionType, 1);
        type->id=g_strdup(id); type->name=g_strdup(name); g_ptr_array_add(disk->types, type);
    }
    g_strfreev(lines); g_free(output);
    return disk->types->len?TRUE:fail(error, "sfdisk returned an unreadable partition type catalogue.");
}

/* Determine the last usable sector from this GPT's actual entry count. */
guint64 capacity_last(const Disk *disk)
{
    guint64 sectors=disk->bytes/disk->sector;
    guint64 reserved=!strcmp(disk->label, "gpt")?2+((guint64)disk->table_length*128+disk->sector-1)/disk->sector:1;
    return sectors>reserved?sectors-reserved:0;
}

/* Query ext filesystem geometry even when it is not mounted. */
static void filesystem_geometry(Partition *part, const char *node)
{
    if (g_str_has_prefix(part->fs, "ext"))
    {
        const char *argv[]={"tune2fs", "-l", node, NULL};
        char *output=NULL;
        if (command_run(argv, NULL, -1, &output, NULL, NULL))
        {
            const char *count=strstr(output, "Block count:"), *size=strstr(output, "Block size:");
            if (count && size) part->fsbytes=g_ascii_strtoull(count+12, NULL, 10)*g_ascii_strtoull(size+11, NULL, 10);
        }
        g_free(output);
    }
    else if (!strcmp(part->fs, "xfs"))
    {
        const char *argv[]={"xfs_db", "-r", "-c", "sb 0", "-c", "p blocksize dblocks", node, NULL};
        char *output=NULL;
        if (command_run(argv, NULL, -1, &output, NULL, NULL))
        {
            char *block=strstr(output, "blocksize = "), *count=strstr(output, "dblocks = ");
            if (block && count) part->fsbytes=g_ascii_strtoull(block+12,NULL,10)*g_ascii_strtoull(count+10,NULL,10);
        }
        g_free(output);
    }
    else if (!strcmp(part->fs, "btrfs"))
    {
        const char *argv[]={"btrfs", "inspect-internal", "dump-super", node, NULL};
        char *output=NULL;
        if (command_run(argv, NULL, -1, &output, NULL, NULL))
        {
            char *size=strstr(output, "dev_item.total_bytes");
            if (size) part->fsbytes=g_ascii_strtoull(size+20,NULL,10);
        }
        g_free(output);
    }
    else if (!strcmp(part->fs, "ntfs") && !part->busy)
    {
        const char *argv[]={"ntfsinfo", "--mft", "--force", node, NULL};
        char *output=NULL;
        if (command_run(argv, NULL, -1, &output, NULL, NULL))
        {
            char *size=strstr(output, "Volume Size in Clusters:"), *cluster=strstr(output,"Cluster Size:");
            if (size && cluster) part->fsbytes=g_ascii_strtoull(size+strlen("Volume Size in Clusters:"),NULL,10)*g_ascii_strtoull(cluster+strlen("Cluster Size:"),NULL,10);
        }
        g_free(output);
    }
}

/* Read signatures directly because udev's cached filesystem data can be stale after mkfs. */
static void filesystem_probe(Partition *part, const char *node)
{
    const char *argv[]={"blkid", "-p", "-o", "export", node, NULL};
    char *output=NULL;
    if (command_run(argv,NULL,-1,&output,NULL,NULL))
    {
        char **lines=g_strsplit(output,"\n",-1);
        for (char **line=lines; *line; line++)
        {
            if (g_str_has_prefix(*line,"TYPE=")) text_set(&part->fs,*line+5);
            if (g_str_has_prefix(*line,"UUID=")) text_set(&part->fsuuid,*line+5);
            if (g_str_has_prefix(*line,"LABEL=")) text_set(&part->fslabel,*line+6);
        }
        g_strfreev(lines);
    }
    g_free(output);
}

/* Load the table and inventory together; the initial dump is the concurrency token. */
Disk *engine_read(const char *path, GError **error)
{
    Disk *disk=disk_new(path);
    int fd=open(path, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) { fail(error, "Cannot open %s: %s", path, g_strerror(errno)); disk_free(disk); return NULL; }
    fstat(fd, &disk->identity);
    disk->image=S_ISREG(disk->identity.st_mode);
    if (disk->image) disk->bytes=disk->identity.st_size;
    else
    {
        if (ioctl(fd, BLKGETSIZE64, &disk->bytes)<0 || ioctl(fd, BLKSSZGET, &disk->sector)<0)
        { close(fd); disk_free(disk); fail(error, "Cannot read device geometry for %s.", path); return NULL; }
    }
    if (disk->bytes<3*disk->sector) { close(fd); disk_free(disk); fail(error,"The device is too small to contain a partition table."); return NULL; }
    const char *dump[]={"sfdisk", "--dump", "/proc/self/fd/3", NULL};
    char *snapshot=NULL;
    command_run(dump, NULL, fd, &snapshot, NULL, NULL);
    text_set(&disk->snapshot, snapshot); g_free(snapshot);
    const char *argv[]={"sfdisk", "--json", "/proc/self/fd/3", NULL};
    char *output=NULL, *diagnostic=NULL;
    gboolean table=command_run(argv, NULL, fd, &output, &diagnostic, NULL);
    text_set(&disk->warning, diagnostic); g_free(diagnostic);
    disk->first=1048576/disk->sector; disk->last=disk->bytes/disk->sector-1;
    if (table)
    {
        JsonParser *parser=parse_json(output, error);
        if (!parser) { g_free(output); close(fd); disk_free(disk); return NULL; }
        JsonObject *object=json_object_get_object_member(json_node_get_object(json_parser_get_root(parser)), "partitiontable");
        text_set(&disk->label, js(object, "label")); text_set(&disk->id, js(object, "id"));
        if (jn(object, "sectorsize")) disk->sector=jn(object, "sectorsize");
        if (jn(object, "firstlba")) disk->first=jn(object, "firstlba");
        if (jn(object, "lastlba")) disk->last=jn(object, "lastlba");
        if (jn(object, "table-length")) disk->table_length=jn(object, "table-length");
        disk->unsupported=strcmp(disk->label, "gpt") && strcmp(disk->label, "dos");
        JsonArray *parts=json_object_has_member(object,"partitions")?json_object_get_array_member(object,"partitions"):NULL;
        for (guint i=0; parts && i<json_array_get_length(parts); i++)
        {
            JsonObject *entry=json_array_get_object_element(parts, i);
            Partition *part=partition_new();
            const char *node=js(entry, "node"), *end=node+strlen(node), *number=end;
            while (number>node && g_ascii_isdigit(number[-1])) number--;
            part->number=g_ascii_strtoull(number, NULL, 10); part->origin=part->number;
            part->start=jn(entry, "start"); part->size=jn(entry, "size");
            guint64 sectors=disk->bytes/disk->sector;
            if (!part->number || !part->size || part->start>=sectors || part->size>sectors-part->start) disk->unsupported=TRUE;
            text_set(&part->type, js(entry, "type")); text_set(&part->uuid, js(entry, "uuid"));
            text_set(&part->name, js(entry, "name")); text_set(&part->attrs, js(entry, "attrs"));
            part->bootable=jn(entry, "bootable");
            if (!strcmp(disk->label, "dos"))
            {
                guint type=strtoul(part->type, NULL, 16);
                if (part->number>4 || type==5 || type==15 || type==0x85) disk->unsupported=TRUE;
            }
            g_ptr_array_add(disk->parts, part);
        }
        g_object_unref(parser);
    }
    g_free(output);
    /* Whole-device filesystems may have a boot sector resembling an empty MBR. */
    if (!disk->parts->len && (!table || !strcmp(disk->label,"dos")))
    {
        Partition *whole=partition_new(); filesystem_probe(whole,path);
        if (*whole->fs)
        {
            text_set(&disk->label,""); text_set(&disk->id,"");
            whole->size=disk->bytes/disk->sector; text_set(&whole->name,whole->fslabel);
            g_ptr_array_add(disk->parts,whole);
        }
        else partition_free(whole);
    }
    disk->original_last=disk->last;
    if (!disk->image)
    {
        JsonParser *parser=inventory(path, error);
        if (!parser) { close(fd); disk_free(disk); return NULL; }
        JsonArray *array=json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "blockdevices");
        for (guint i=0; i<json_array_get_length(array); i++)
        {
            JsonObject *entry=json_array_get_object_element(array, i);
            char *canonical=realpath(js(entry, "path"), NULL);
            const char *node=canonical?canonical:js(entry, "path"), *kind=js(entry, "type");
            char *mount=mounts(entry);
            gboolean busy=*mount || has_holders(js(entry, "maj:min"));
            if (!strcmp(node, path))
            {
                text_set(&disk->kind, kind); text_set(&disk->transport, js(entry, "tran"));
                text_set(&disk->serial, js(entry, "serial")); text_set(&disk->wwn, js(entry, "wwn"));
                text_set(&disk->majmin, js(entry, "maj:min"));
                if (*js(entry, "model")) { text_set(&disk->title, js(entry, "model")); g_strstrip(disk->title); }
                disk->readonly=jn(entry, "ro");
            }
            disk->busy|=busy;
            for (guint j=0; j<disk->parts->len; j++)
            {
                Partition *part=disk->parts->pdata[j];
                char *expected=part->number?g_strdup_printf("%s%s%u", path, g_ascii_isdigit(path[strlen(path)-1])?"p":"", part->number):g_strdup(path);
                if (!strcmp(expected, node))
                {
                    text_set(&part->fs, js(entry, "fstype")); text_set(&part->fsuuid, js(entry, "uuid"));
                    text_set(&part->fslabel, js(entry, "label"));
                    text_set(&part->mount, mount); part->busy=busy;
                    part->readonly=jn(entry, "ro");
                    part->fsbytes=jn(entry, "fssize"); filesystem_probe(part, node); filesystem_geometry(part, node);
                }
                g_free(expected);
            }
            g_free(mount); free(canonical);
        }
        g_object_unref(parser);
    }
    char *after=NULL;
    command_run(dump, NULL, fd, &after, NULL, NULL); close(fd);
    gboolean unchanged=!strcmp(after?after:"", disk->snapshot); g_free(after);
    if (!unchanged) { disk_free(disk); fail(error, "The partition table changed while it was being read. Refresh and try again."); return NULL; }
    g_ptr_array_unref(disk->base); disk->base=parts_copy(disk->parts);
    if (!load_types(disk, error)) { disk_free(disk); return NULL; }
    if (disk->unsupported) text_set(&disk->warning, "This layout is available for inspection only. It contains unsupported or inconsistent partition entries.");
    return disk;
}

/* Compressed RAM is never a disk target; loop devices require an explicit option. */
static gboolean visible_device(const char *path, const char *type, gboolean include_loop)
{
    if (g_str_has_prefix(path,"/dev/zram")) return FALSE;
    return include_loop || strcmp(type,"loop");
}

/* Inventory includes usable virtual devices while retaining busy physical parents. */
GPtrArray *engine_scan(const char *only, gboolean include_loop, GError **error)
{
    GPtrArray *disks=g_ptr_array_new_with_free_func(disk_free);
    if (only)
    {
        Disk *disk=engine_read(only, error);
        if (!disk) { g_ptr_array_unref(disks); return NULL; }
        if (visible_device(disk->path,disk->kind,include_loop)) g_ptr_array_add(disks,disk);
        else disk_free(disk);
        return disks;
    }
    JsonParser *parser=inventory(NULL, error);
    if (!parser) { g_ptr_array_unref(disks); return NULL; }
    JsonArray *array=json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "blockdevices");
    for (guint i=0; i<json_array_get_length(array); i++)
    {
        JsonObject *entry=json_array_get_object_element(array, i);
        const char *type=js(entry, "type"), *path=js(entry, "path");
        if (!strcmp(type, "part") || !strcmp(type, "rom") || !jn(entry, "size")) continue;
        if (!visible_device(path,type,include_loop)) continue;
        char *canonical=realpath(path, NULL);
        Disk *disk=canonical?engine_read(canonical, NULL):NULL;
        free(canonical);
        if (disk) g_ptr_array_add(disks, disk);
    }
    g_object_unref(parser);
    return disks;
}

/* An explicit new-table action replaces geometry and resets the draft catalogue. */
gboolean engine_new_table(Disk *disk, const char *label, GError **error)
{
    if (strcmp(label, "gpt") && strcmp(label, "dos")) return fail(error, "Unsupported partition table.");
    text_set(&disk->label, label);
    char *id=!strcmp(label, "gpt")?g_uuid_string_random():g_strdup_printf("0x%08x", g_random_int());
    text_set(&disk->id, id); g_free(id);
    disk->table_length=128; disk->first=1048576/disk->sector; disk->last=capacity_last(disk);
    disk->new_table=TRUE; disk->repair=FALSE; disk->unsupported=FALSE;
    g_ptr_array_set_size(disk->parts, 0);
    return load_types(disk, error);
}

/* Compare the pinned file description with the model chosen in the UI. */
static gboolean same_identity(const Disk *disk, int fd, GError **error)
{
    struct stat current;
    if (fstat(fd, &current)<0) return fail(error, "The selected device is no longer available.");
    gboolean same=current.st_dev==disk->identity.st_dev && current.st_ino==disk->identity.st_ino && current.st_rdev==disk->identity.st_rdev;
    guint64 bytes=0;
    if (S_ISREG(current.st_mode)) bytes=current.st_size;
    else if (ioctl(fd, BLKGETSIZE64, &bytes)<0) same=FALSE;
    return same && bytes==disk->bytes?TRUE:fail(error, "The selected device was replaced or its capacity changed. Refresh before applying changes.");
}

/* Refresh usage only while the selected device and original table still match. */
Disk *engine_recheck(Disk *disk, GError **error)
{
    int fd=open(disk->path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) { fail(error,"Cannot open %s: %s",disk->path,g_strerror(errno)); return NULL; }
    Disk *current=same_identity(disk,fd,error)?engine_read(disk->path,error):NULL;
    if (current)
    {
        gboolean same=same_identity(current,fd,error);
        if (same && (strcmp(current->snapshot,disk->snapshot) || strcmp(current->serial,disk->serial) || strcmp(current->wwn,disk->wwn)))
            same=fail(error,"This device or its partition table changed since the plan was created. Refresh and review the new layout.");
        if (!same) { disk_free(current); current=NULL; }
    }
    close(fd); return current;
}

/* Write to a private backup directory before modifying any table or filesystem. */
static char *make_backup(Disk *disk, const char *root, int fd, GString *log, GError **error)
{
    if (g_mkdir_with_parents(root, 0700)<0) { fail(error, "Cannot create the partition-table backup directory: %s", g_strerror(errno)); return NULL; }
    char *directory=g_build_filename(root, "backup-XXXXXX", NULL);
    if (!g_mkdtemp(directory)) { fail(error, "Cannot create a backup: %s", g_strerror(errno)); g_free(directory); return NULL; }
    char *dump=g_build_filename(directory, "layout.sfdisk", NULL);
    gboolean saved=g_file_set_contents_full(dump, disk->snapshot, -1, G_FILE_SET_CONTENTS_CONSISTENT|G_FILE_SET_CONTENTS_DURABLE, 0600, error); g_free(dump);
    if (saved && *disk->snapshot)
    {
        char *prefix=g_build_filename(directory, "table", NULL);
        const char *argv[]={"sfdisk", "--backup-pt-sectors", "--backup-file", prefix, "/proc/self/fd/3", NULL};
        saved=command_run(argv, NULL, fd, NULL, NULL, error); g_free(prefix);
    }
    if (!saved) { g_free(directory); return NULL; }
    g_string_append_printf(log, "Partition-table backup: %s\n", directory);
    return directory;
}

/* Re-read the on-disk table and compare the exact requested extents before mkfs. */
static gboolean verify_layout(Disk *wanted, Disk *actual, GError **error)
{
    if (strcmp(wanted->label, actual->label) || wanted->parts->len!=actual->parts->len)
        return fail(error, "The written partition table differs from the requested layout. No filesystems were created.");
    if (wanted->sector!=actual->sector || (*wanted->id && g_ascii_strcasecmp(wanted->id,actual->id)))
        return fail(error,"The written table has an unexpected identifier or sector size.");
    if (!strcmp(wanted->label,"gpt") && (wanted->first!=actual->first || wanted->last!=actual->last))
        return fail(error,"The GPT usable range differs from the requested layout.");
    for (guint i=0; i<wanted->parts->len; i++)
    {
        Partition *part=wanted->parts->pdata[i], *found=find_part(actual->parts, part->number);
        if (!found || found->start!=part->start || found->size!=part->size || g_ascii_strcasecmp(found->type, part->type))
            return fail(error, "Partition %u does not match the requested geometry or type. Filesystem operations were stopped.", part->number);
        if (*part->uuid && g_ascii_strcasecmp(part->uuid, found->uuid)) return fail(error, "Partition %u has an unexpected UUID.", part->number);
        if (!strcmp(wanted->label,"gpt") && (strcmp(part->name,found->name) || strcmp(part->attrs,found->attrs)))
            return fail(error,"Partition %u's name or attributes were not preserved.",part->number);
        if (!strcmp(wanted->label,"dos") && part->bootable!=found->bootable) return fail(error,"Partition %u's boot flag was not preserved.",part->number);
    }
    return TRUE;
}

/* Open with the requested access and verify the intended kernel geometry. */
static int open_partition(Disk *disk, Partition *part, int mode, gboolean exclusive, GError **error)
{
    if (disk->image) { fail(error, "Filesystem operations require a loop or block device, not a regular image file."); return -1; }
    char *path=part->number?g_strdup_printf("%s%s%u", disk->path, g_ascii_isdigit(disk->path[strlen(disk->path)-1])?"p":"", part->number):g_strdup(disk->path);
    int fd=open(path, mode|O_CLOEXEC|O_NOFOLLOW|(exclusive?O_EXCL:0));
    if (fd<0) { fail(error, "Cannot open %s for this operation: %s", path, g_strerror(errno)); g_free(path); return -1; }
    /* Let the native tool acquire its own exclusive claim, keeping a pinned descriptor. */
    if (exclusive)
    {
        close(fd); fd=open(path, mode|O_CLOEXEC|O_NOFOLLOW);
        if (fd<0) { fail(error, "The selected partition is no longer available."); g_free(path); return -1; }
    }
    struct stat identity;
    fstat(fd, &identity);
    char *sys=g_strdup_printf("/sys/dev/block/%u:%u/start", major(identity.st_rdev), minor(identity.st_rdev));
    char *start=NULL;
    gboolean read=g_file_get_contents(sys, &start, NULL, NULL); g_free(sys);
    guint64 size=0;
    gboolean correct=ioctl(fd, BLKGETSIZE64, &size)==0 && size==part->size*disk->sector;
    if (part->number) correct=correct && read && g_ascii_strtoull(start, NULL, 10)==part->start*disk->sector/512;
    if (part->number)
    {
        char *parent=g_strdup_printf("/sys/dev/block/%u:%u/../dev", major(identity.st_rdev), minor(identity.st_rdev)), *majmin=NULL;
        correct=correct && g_file_get_contents(parent,&majmin,NULL,NULL) && !strcmp(g_strstrip(majmin),disk->majmin);
        g_free(parent); g_free(majmin);
    }
    g_free(start); g_free(path);
    if (!correct) { close(fd); fail(error, "The kernel has not accepted partition %u's new boundaries. Filesystem operations were stopped.", part->number); return -1; }
    return fd;
}

/* Try ordinary unmounts on verified partitions, retaining busy mounts and other uses. */
gboolean engine_unmount(Disk *disk, GError **error)
{
    Disk *current=engine_recheck(disk,error); if (!current) return FALSE;
    int device=open(disk->path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (device<0) { disk_free(current); return fail(error,"Cannot open %s: %s",disk->path,g_strerror(errno)); }
    if (!same_identity(current,device,error)) { close(device); disk_free(current); return FALSE; }
    GString *errors=g_string_new(NULL);
    for (guint i=current->parts->len; i>0; i--)
    {
        Partition *part=current->parts->pdata[i-1]; if (part->mount[0]!='/') continue;
        GError *local=NULL; int fd=open_partition(current,part,O_RDONLY,FALSE,&local);
        if (fd>=0)
        {
            const char *argv[]={"umount","--all-targets","--","/proc/self/fd/3",NULL};
            command_run(argv,NULL,fd,NULL,NULL,&local); close(fd);
        }
        if (local) { g_string_append_printf(errors,"%s%s",errors->len?"\n":"",local->message); g_clear_error(&local); }
    }
    close(device); disk_free(current);
    gboolean result=!errors->len;
    if (!result) fail(error,"%s",errors->str);
    g_string_free(errors,TRUE); return result;
}

/* Recheck filesystem identity and mounts immediately before a destructive operation. */
static gboolean verify_filesystem(Partition *part, int fd, GError **error)
{
    if (!part->origin || !*part->fsuuid) return TRUE;
    const char *argv[]={"blkid", "-p", "-s", "UUID", "-o", "value", "/proc/self/fd/3", NULL};
    char *output=NULL;
    gboolean result=command_run(argv, NULL, fd, &output, NULL, error);
    if (result && strcmp(g_strstrip(output), part->fsuuid)) result=fail(error, "The filesystem on partition %u changed since it was selected.", part->number);
    g_free(output);
    return result;
}

/* Keep the intentionally small formatter adapters separate from runtime tool discovery. */
static gboolean format_partition(Partition *part, int fd, GString *log, gboolean allow_busy, GError **error)
{
    Filesystem *fs=find_filesystem(part->format);
    if (!fs || !fs->format_available) return fail(error, "The selected filesystem formatter is not installed.");
    const char *argv[8]={fs->formatter, NULL};
    if (!strcmp(fs->id, "ext4"))
    {
        argv[1]="-F"; argv[2]="/proc/self/fd/3";
        if (allow_busy && part->busy) { argv[2]="-F"; argv[3]="/proc/self/fd/3"; }
    }
    else if (!strcmp(fs->id, "xfs") || !strcmp(fs->id, "btrfs")) { argv[1]="-f"; argv[2]="/proc/self/fd/3"; }
    else if (!strcmp(fs->id, "ntfs")) { argv[1]="-F"; argv[2]="-Q"; argv[3]="/proc/self/fd/3"; }
    else if (!strcmp(fs->id, "vfat")) { argv[1]="-F"; argv[2]="32"; argv[3]="/proc/self/fd/3"; }
    else argv[1]="/proc/self/fd/3";
    char *output=NULL;
    gboolean result=command_run(argv, NULL, fd, &output, NULL, error);
    if (output) g_string_append(log, output);
    g_free(output);
    if (!result) return FALSE;
    const char *probe[]={"blkid", "-p", "-s", "TYPE", "-o", "value", "/proc/self/fd/3", NULL};
    if (!command_run(probe, NULL, fd, &output, NULL, error)) return FALSE;
    result=!strcmp(g_strstrip(output), fs->id); g_free(output);
    return result?TRUE:fail(error, "Formatting finished but the resulting filesystem could not be verified.");
}

/* Filesystem checks accept e2fsck's corrected-success status, never its error bits. */
static gboolean check_ext(int fd, GError **error)
{
    const char *argv[]={"e2fsck", "-f", "-p", "/proc/self/fd/3", NULL};
    GError *check=NULL;
    if (command_run(argv, NULL, fd, NULL, NULL, &check)) return TRUE;
    if (g_error_matches(check, G_SPAWN_EXIT_ERROR, 1)) { g_clear_error(&check); return TRUE; }
    g_propagate_error(error, check); return FALSE;
}

/* Mount only the selected single-device filesystem when its grower requires it. */
static gboolean grow_partition(Partition *part, int fd, GString *log, GError **error)
{
    const char *id=g_str_has_prefix(part->fs, "ext")?"ext4":part->fs;
    Filesystem *fs=find_filesystem(id);
    if (!fs || !fs->grow_available) return fail(error, "No supported grow tool is installed for %s.", part->fs);
    if (!strcmp(id, "ext4") || !strcmp(id, "ntfs"))
    {
        if (!strcmp(id, "ext4") && !part->busy && !check_ext(fd, error)) return FALSE;
        const char *argv[]={fs->grower, !strcmp(id, "ntfs")?"--no-progress-bar":"/proc/self/fd/3", !strcmp(id, "ntfs")?"/proc/self/fd/3":NULL, NULL};
        char *output=NULL;
        gboolean result=command_run(argv, !strcmp(id, "ntfs")?"y\n":NULL, fd, &output, NULL, error);
        if (output) g_string_append(log, output);
        g_free(output); return result;
    }
    if (!strcmp(id, "btrfs"))
    {
        const char *probe[]={"btrfs", "inspect-internal", "dump-super", "/proc/self/fd/3", NULL};
        char *output=NULL;
        if (!command_run(probe, NULL, fd, &output, NULL, error)) return FALSE;
        char *devices=strstr(output, "num_devices");
        gboolean single=devices && g_ascii_strtoull(devices+11, NULL, 10)==1;
        g_free(output);
        if (!single) return fail(error, "Growing a multi-device Btrfs filesystem is not supported.");
    }
    gboolean temporary=!*part->mount;
    char *directory=NULL;
    if (!temporary)
    {
        const char *find[]={"findmnt","--json","--first-only","--evaluate","--nofsroot","--source","/proc/self/fd/3","--output","TARGET,SOURCE,FSTYPE",NULL};
        char *output=NULL;
        if (!command_run(find,NULL,fd,&output,NULL,error)) return FALSE;
        JsonParser *parser=parse_json(output,error); g_free(output);
        if (!parser) return FALSE;
        JsonArray *array=json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)),"filesystems");
        JsonObject *mount=json_array_get_object_element(array,0); struct stat identity,source; fstat(fd,&identity);
        /* Btrfs mounts have an anonymous filesystem device ID; verify their block source. */
        gboolean correct=stat(js(mount,"source"),&source)==0 && source.st_rdev==identity.st_rdev;
        correct=correct && !strcmp(js(mount,"fstype"),id);
        if (correct) directory=g_strdup(js(mount,"target"));
        g_object_unref(parser);
        if (!correct) return fail(error,"The mounted filesystem no longer matches the selected partition.");
    }
    else directory=g_dir_make_tmp("partitions-mount-XXXXXX", error);
    if (!directory) return FALSE;
    const char *mount[]={"mount", "--no-mtab", "-t", id, "-o", "rw,nosuid,nodev,noexec", "/proc/self/fd/3", directory, NULL};
    gboolean result=!temporary || command_run(mount, NULL, fd, NULL, NULL, error);
    if (result)
    {
        const char *xfs[]={"xfs_growfs", "-d", directory, NULL};
        const char *btrfs[]={"btrfs", "filesystem", "resize", "max", directory, NULL};
        char *output=NULL;
        result=command_run(!strcmp(id, "xfs")?xfs:btrfs, NULL, fd, &output, NULL, error);
        if (output) g_string_append(log, output);
        g_free(output);
        if (temporary)
        {
            const char *unmount[]={"umount", directory, NULL}; GError *unmount_error=NULL;
            if (!command_run(unmount, NULL, -1, NULL, NULL, &unmount_error))
            {
                if (result) g_propagate_error(error, unmount_error); else g_error_free(unmount_error);
                result=FALSE;
            }
        }
    }
    if (temporary) rmdir(directory);
    g_free(directory); return result;
}

/* On an acknowledged in-use disk, write metadata without a global kernel reread. */
static gboolean write_table(const char *script, int fd, gboolean busy, gboolean dry, GString *log, GError **error)
{
    const char *argv[12]={"sfdisk","--lock=no","--wipe=never","--wipe-partitions=never",NULL}; guint count=4;
    if (busy) { argv[count++]="--no-reread"; argv[count++]="--no-tell-kernel"; }
    if (dry) argv[count++]="--no-act";
    argv[count++]="/proc/self/fd/3"; argv[count]=NULL;
    char *output=NULL; gboolean result=command_run(argv,script,fd,&output,NULL,error);
    if (output && !dry) g_string_append(log,output);
    g_free(output); return result;
}

/* A new table invalidates whole-device signatures; retain table boot code and partition contents. */
static gboolean clear_device_signatures(int fd, const char *backup, GString *log, GError **error)
{
    const char *probe[]={"wipefs","--json","--output","TYPE,USAGE","/proc/self/fd/3",NULL}; char *output=NULL;
    if (!command_run(probe,NULL,fd,&output,NULL,error)) { g_free(output); return FALSE; }
    JsonParser *parser=parse_json(output,error); g_free(output);
    if (!parser) return FALSE;
    JsonArray *signatures=json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)),"signatures");
    GString *types=g_string_new(NULL);
    for (guint i=0;i<json_array_get_length(signatures);i++)
    {
        JsonObject *entry=json_array_get_object_element(signatures,i);
        if (strcmp(js(entry,"usage"),"partition-table")) g_string_append_printf(types,"%s%s",types->len?",":"",js(entry,"type"));
    }
    gboolean result=TRUE;
    if (types->len)
    {
        char *save=g_strdup_printf("--backup=%s",backup);
        const char *wipe[]={"wipefs","--all","--force","--lock=no",save,"--types",types->str,"/proc/self/fd/3",NULL};
        output=NULL; result=command_run(wipe,NULL,fd,&output,NULL,error);
        if (output) g_string_append(log,output);
        g_free(output); g_free(save);
    }
    g_string_free(types,TRUE); g_object_unref(parser); return result;
}

/* Update individual kernel entries; mounted deletions can remain active until restart. */
static void update_kernel(Disk *disk, int fd, ApplyOptions *options, GString *log)
{
    for (guint pass=0;pass<2;pass++)
    {
        GPtrArray *parts=pass?disk->parts:disk->base;
        for (guint i=0;i<parts->len;i++)
        {
            Partition *part=parts->pdata[i], *other=find_part(pass?disk->base:disk->parts,part->number);
            const char *operation=NULL;
            if (!pass && part->number && (disk->new_table || !other || !other->origin)) operation="--delete";
            if (pass && (!part->origin || disk->new_table)) operation="--add";
            else if (pass && other && part->size!=other->size) operation="--update";
            if (!operation) continue;
            char *number=g_strdup_printf("%u",part->number); const char *argv[]={"partx",operation,"--nr",number,"/proc/self/fd/3",NULL};
            GError *error=NULL;
            if (!command_run(argv,NULL,fd,NULL,NULL,&error))
            {
                options->reboot_required=TRUE;
                g_string_append_printf(log,"Kernel update deferred for partition %u: %s\n",part->number,error->message); g_error_free(error);
            }
            g_free(number);
        }
    }
    if (options->reboot_required) g_string_append(log,"Restart required: the on-disk table is updated, but the kernel retains some previous partition entries.\n");
}

/* Apply a validated plan to the pinned physical device, stopping at the first failure. */
static gboolean lock_disk(int fd, GError **error)
{
    gint64 deadline=g_get_monotonic_time()+2*G_USEC_PER_SEC;
    while (flock(fd,LOCK_EX|LOCK_NB)<0)
    {
        if (errno!=EWOULDBLOCK || g_get_monotonic_time()>=deadline) return fail(error,"Another program is using this device. Close it and try again.");
        g_usleep(20000);
    }
    return TRUE;
}

/* Pin, validate, back up and apply exactly the disk plan approved by the user. */
gboolean engine_apply(Disk *disk, const char *backup_root, GString *log, ApplyOptions *options, GError **error)
{
    if (!validate_plan(disk, error)) return FALSE;
    int fd=open(disk->path, O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return fail(error, "Cannot open %s for writing: %s", disk->path, g_strerror(errno));
    gboolean result=same_identity(disk, fd, error);
    if (result) result=lock_disk(fd,error);
    Disk *current=result?engine_recheck(disk,error):NULL;
    if (result && !current) result=FALSE;
    gboolean active=current && current->busy;
    if (result && active && !options->allow_busy) result=fail(error,"This disk is in use. Review and explicitly acknowledge the risk before applying changes.");
    if (result)
        for (guint i=0;i<disk->parts->len;i++)
        {
            Partition *part=disk->parts->pdata[i], *live=find_part(current->parts,part->number);
            if (!live) continue;
            part->busy=live->busy; text_set(&part->mount,live->mount);
        }
    if (current) disk_free(current);
    for (guint i=0; result && i<disk->parts->len; i++)
    {
        Partition *part=disk->parts->pdata[i];
        const char *id=*part->format?part->format:part->fs;
        if (g_str_has_prefix(id,"ext")) id="ext4";
        Filesystem *fs=find_filesystem(id);
        if (*part->format && (!fs || !fs->format_available)) result=fail(error, "The requested filesystem formatter is unavailable.");
        if (part->grow && (!fs || !fs->grow_available)) result=fail(error, "The requested filesystem grower is unavailable.");
        /* Reject mounted or unhealthy NTFS before enlarging its partition on disk. */
        if (result && part->grow && !strcmp(id,"ntfs"))
        {
            Partition *original=find_part(disk->base,part->origin); int part_fd=open_partition(disk,original,O_RDWR,TRUE,error);
            result=part_fd>=0 && verify_filesystem(original,part_fd,error);
            const char *check[]={"ntfsresize","--info","/proc/self/fd/3",NULL};
            if (result) result=command_run(check,NULL,part_fd,NULL,NULL,error);
            if (part_fd>=0) close(part_fd);
        }

    }
    char *backup=result?make_backup(disk, backup_root, fd, log, error):NULL;
    if (result && !backup) result=FALSE;
    if (result && layout_changed(disk))
    {
        char *script=disk_script(disk);
        result=write_table(script,fd,active,TRUE,log,error);
        if (result && disk->new_table) result=clear_device_signatures(fd,backup,log,error);
        if (result) result=write_table(script,fd,active,FALSE,log,error);
        g_free(script);
        if (result && fsync(fd)<0) result=fail(error, "The partition table could not be flushed to the device.");
        if (result && active && !disk->image) update_kernel(disk,fd,options,log);
    }
    if (result)
    {
        Disk *actual=engine_read(disk->path, error);
        result=actual && same_identity(disk, fd, error);
        if (result) result=verify_layout(disk, actual, error);
        if (actual) disk_free(actual);
    }
    for (guint i=0; result && i<disk->parts->len; i++)
    {
        Partition *part=disk->parts->pdata[i];
        if (!*part->format && !part->grow) continue;
        result=same_identity(disk, fd, error);
        gboolean needs_mount=part->grow && (!strcmp(part->fs, "xfs") || !strcmp(part->fs, "btrfs"));
        gboolean exclusive=!needs_mount && !(options->allow_busy && part->busy);
        int part_fd=result?open_partition(disk, part, O_RDWR, exclusive, error):-1;
        if (part_fd<0) { result=FALSE; break; }
        result=verify_filesystem(part, part_fd, error) && same_identity(disk, fd, error);
        if (result && *part->format) result=format_partition(part, part_fd, log, options->allow_busy, error);
        else if (result && part->grow) result=grow_partition(part, part_fd, log, error);
        if (result && fsync(part_fd)<0) result=fail(error, "Filesystem changes could not be flushed to the device.");
        close(part_fd);
    }
    close(fd); g_free(backup);
    if (!result) g_string_append(log, "Stopped after an error. Completed writes are retained. Refresh to inspect the actual disk; no automatic rollback was attempted.\n");
    return result;
}

/* Check the partition table without changing it or running filesystem repair. */
gboolean engine_check(Disk *disk, char **output, GError **error)
{
    const char *argv[]={"sfdisk", "--verify", disk->path, NULL};
    char *text=NULL,*diagnostic=NULL; gboolean result=command_run(argv,NULL,-1,&text,&diagnostic,error);
    *output=g_strconcat(text?text:"",diagnostic?diagnostic:"",NULL); g_free(text); g_free(diagnostic); return result;
}
