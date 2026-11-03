#include "partition-manager.h"
#include <stdarg.h>
#include <string.h>

/* Return actionable errors through the same path as command failures. */
gboolean fail(GError **error, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *message=g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
    g_free(message);
    return FALSE;
}

/* Keep every string owned by its model, including empty optional fields. */
Partition *partition_new(void)
{
    Partition *part=g_new0(Partition, 1);
    part->type=g_strdup(""); part->uuid=g_strdup(""); part->name=g_strdup("");
    part->attrs=g_strdup(""); part->fs=g_strdup(""); part->fsuuid=g_strdup("");
    part->fslabel=g_strdup(""); part->mount=g_strdup(""); part->format=g_strdup("");
    return part;
}

/* Snapshots own their strings so Undo cannot alter the original disk state. */
Partition *partition_copy(const Partition *part)
{
    Partition *copy=g_memdup2(part, sizeof *part);
    copy->type=g_strdup(part->type); copy->uuid=g_strdup(part->uuid);
    copy->name=g_strdup(part->name); copy->attrs=g_strdup(part->attrs);
    copy->fs=g_strdup(part->fs); copy->fsuuid=g_strdup(part->fsuuid);
    copy->fslabel=g_strdup(part->fslabel);
    copy->mount=g_strdup(part->mount); copy->format=g_strdup(part->format);
    return copy;
}

/* Release an individual partition or an Undo copy. */
void partition_free(gpointer data)
{
    Partition *part=data;
    g_free(part->type); g_free(part->uuid); g_free(part->name); g_free(part->attrs);
    g_free(part->fs); g_free(part->fsuuid); g_free(part->mount); g_free(part->format);
    g_free(part->fslabel);
    g_free(part);
}

/* Copy a layout while preserving the partition numbers used by sfdisk. */
GPtrArray *parts_copy(GPtrArray *parts)
{
    GPtrArray *copy=g_ptr_array_new_with_free_func(partition_free);
    for (guint i=0; i<parts->len; i++) g_ptr_array_add(copy, partition_copy(parts->pdata[i]));
    return copy;
}

/* Release the runtime catalogue rather than keeping a handwritten GUID list. */
static void type_free(gpointer data)
{
    PartitionType *type=data;
    g_free(type->id); g_free(type->name); g_free(type);
}

/* Create an empty, fully owned disk model for inventory or file-image tests. */
Disk *disk_new(const char *path)
{
    Disk *disk=g_new0(Disk, 1);
    disk->path=g_strdup(path); disk->title=g_path_get_basename(path);
    disk->kind=g_strdup("disk"); disk->transport=g_strdup(""); disk->serial=g_strdup("");
    disk->wwn=g_strdup(""); disk->majmin=g_strdup(""); disk->label=g_strdup("");
    disk->id=g_strdup(""); disk->snapshot=g_strdup(""); disk->warning=g_strdup("");
    disk->sector=512; disk->table_length=128;
    disk->parts=g_ptr_array_new_with_free_func(partition_free);
    disk->base=g_ptr_array_new_with_free_func(partition_free);
    disk->types=g_ptr_array_new_with_free_func(type_free);
    return disk;
}

/* Disk snapshots include geometry, intent and identity, never live handles. */
Disk *disk_copy(const Disk *disk)
{
    Disk *copy=g_memdup2(disk, sizeof *disk);
    copy->path=g_strdup(disk->path); copy->title=g_strdup(disk->title);
    copy->kind=g_strdup(disk->kind); copy->transport=g_strdup(disk->transport);
    copy->serial=g_strdup(disk->serial); copy->wwn=g_strdup(disk->wwn); copy->majmin=g_strdup(disk->majmin);
    copy->label=g_strdup(disk->label); copy->id=g_strdup(disk->id);
    copy->snapshot=g_strdup(disk->snapshot); copy->warning=g_strdup(disk->warning);
    copy->parts=parts_copy(disk->parts); copy->base=parts_copy(disk->base);
    copy->types=g_ptr_array_new_with_free_func(type_free);
    for (guint i=0; i<disk->types->len; i++)
    {
        PartitionType *old=disk->types->pdata[i], *type=g_new0(PartitionType, 1);
        type->id=g_strdup(old->id); type->name=g_strdup(old->name);
        g_ptr_array_add(copy->types, type);
    }
    return copy;
}

/* Release inventory entries and discarded plans. */
void disk_free(gpointer data)
{
    Disk *disk=data;
    g_free(disk->path); g_free(disk->title); g_free(disk->kind); g_free(disk->transport);
    g_free(disk->serial); g_free(disk->wwn); g_free(disk->majmin); g_free(disk->label);
    g_free(disk->id); g_free(disk->snapshot); g_free(disk->warning);
    g_ptr_array_unref(disk->parts); g_ptr_array_unref(disk->base); g_ptr_array_unref(disk->types);
    g_free(disk);
}

/* Partition numbers stay stable when earlier entries are deleted. */
Partition *find_part(GPtrArray *parts, guint number)
{
    for (guint i=0; i<parts->len; i++)
    {
        Partition *part=parts->pdata[i];
        if (part->number==number) return part;
    }
    return NULL;
}

/* Sorting by location also makes overlap validation independent of row order. */
static gint compare_start(gconstpointer a, gconstpointer b)
{
    const Partition *left=*(Partition *const *)a, *right=*(Partition *const *)b;
    return (left->start>right->start)-(left->start<right->start);
}

/* Derive free intervals from occupied extents, consolidating alignment gaps. */
GArray *free_regions(const Disk *disk)
{
    GArray *regions=g_array_new(FALSE, FALSE, sizeof(Region));
    GPtrArray *sorted=parts_copy(disk->parts);
    g_ptr_array_sort(sorted, compare_start);
    guint64 cursor=disk->first;
    for (guint i=0; i<sorted->len; i++)
    {
        Partition *part=sorted->pdata[i];
        if (part->start>cursor)
        {
            Region region={cursor, part->start-cursor};
            g_array_append_val(regions, region);
        }
        cursor=MAX(cursor, part->start+part->size);
    }
    if (cursor<=disk->last)
    {
        Region region={cursor, disk->last+1-cursor};
        g_array_append_val(regions, region);
    }
    g_ptr_array_unref(sorted);
    return regions;
}

/* Existing data can grow only to the next occupied interval. */
guint64 grow_limit(const Disk *disk, const Partition *part)
{
    guint64 end=disk->last+1;
    for (guint i=0; i<disk->parts->len; i++)
    {
        Partition *next=disk->parts->pdata[i];
        if (next!=part && next->start>=part->start+part->size) end=MIN(end, next->start);
    }
    return end-part->start;
}

/* Only metadata differences require rewriting the partition table. */
gboolean layout_changed(const Disk *disk)
{
    if (disk->new_table || disk->repair || disk->parts->len!=disk->base->len) return TRUE;
    for (guint i=0; i<disk->parts->len; i++)
    {
        /* Number zero identifies an existing whole-device filesystem in both snapshots. */
        Partition *part=disk->parts->pdata[i], *old=find_part(disk->base, part->origin);
        if (!old || part->start!=old->start || part->size!=old->size) return TRUE;
        if (strcmp(part->type, old->type) || strcmp(part->name, old->name)) return TRUE;
    }
    return FALSE;
}

/* Formatting and filesystem-only growth are independent user requests. */
gboolean disk_changed(const Disk *disk)
{
    if (layout_changed(disk)) return TRUE;
    for (guint i=0; i<disk->parts->len; i++)
    {
        Partition *part=disk->parts->pdata[i];
        if (*part->format || part->grow) return TRUE;
    }
    return FALSE;
}

/* Validate the complete intended layout before any backup or write command. */
gboolean validate_plan(const Disk *disk, GError **error)
{
    if (disk->readonly || disk->unsupported) return fail(error, "This device is read-only or uses an unsupported layout.");
    if (strcmp(disk->label, "gpt") && strcmp(disk->label, "dos")) return fail(error, "Choose a GPT or MBR partition table first.");
    guint maximum=!strcmp(disk->label, "dos")?4:disk->table_length;
    GPtrArray *sorted=parts_copy(disk->parts);
    g_ptr_array_sort(sorted, compare_start);
    guint64 end=disk->first;
    gboolean valid=TRUE;
    for (guint i=0; i<sorted->len && valid; i++)
    {
        Partition *part=sorted->pdata[i], *old=part->origin?find_part(disk->base, part->origin):NULL;
        if (!part->number || part->number>maximum) valid=fail(error, "There are no free partition entries in this table.");
        else if (!part->size || part->start<end || part->start>disk->last || part->size>disk->last-part->start+1)
            valid=fail(error, "Partitions overlap or extend outside the usable disk space.");
        else if (old && !disk->new_table && (part->start!=old->start || part->size<old->size))
            valid=fail(error, "Existing partitions cannot be moved or shrunk. Delete the partition first.");
        else if (part->readonly && (*part->format || part->grow)) valid=fail(error, "The selected filesystem is read-only.");
        for (guint j=0; j<i && valid; j++)
            if (((Partition *)sorted->pdata[j])->number==part->number) valid=fail(error, "Partition numbers must be unique.");
        end=part->start+part->size;
    }
    g_ptr_array_unref(sorted);
    return valid;
}

/* Quote script strings without allowing names to create new script records. */
static char *script_quote(const char *value)
{
    GString *text=g_string_new("\"");
    for (const char *c=value; *c; c++)
    {
        if (*c=='\\' || *c=='"') g_string_append_c(text, '\\');
        if (*c=='\n' || *c=='\r') g_string_append_c(text, ' ');
        else g_string_append_c(text, *c);
    }
    g_string_append_c(text, '"');
    return g_string_free(text, FALSE);
}

/* Preserve table IDs, GPT attributes and partition numbers in sfdisk input. */
char *disk_script(const Disk *disk)
{
    GString *text=g_string_new(NULL);
    g_string_append_printf(text, "label: %s\nunit: sectors\nsector-size: %u\n", disk->label, disk->sector);
    if (*disk->id) g_string_append_printf(text, "label-id: %s\n", disk->id);
    if (!strcmp(disk->label, "gpt"))
        g_string_append_printf(text, "first-lba: %"G_GUINT64_FORMAT"\nlast-lba: %"G_GUINT64_FORMAT"\ntable-length: %u\n", disk->first, disk->last, disk->table_length);
    for (guint number=1; number<=MAX(4, disk->table_length); number++)
    {
        Partition *part=find_part(disk->parts, number);
        if (!part) continue;
        g_string_append_printf(text, "%u: start=%"G_GUINT64_FORMAT", size=%"G_GUINT64_FORMAT", type=%s", number, part->start, part->size, part->type);
        if (*part->uuid) g_string_append_printf(text, ", uuid=%s", part->uuid);
        if (*part->name && !strcmp(disk->label, "gpt"))
        {
            char *name=script_quote(part->name);
            g_string_append_printf(text, ", name=%s", name); g_free(name);
        }
        if (*part->attrs)
        {
            char *attrs=script_quote(part->attrs);
            g_string_append_printf(text, ", attrs=%s", attrs); g_free(attrs);
        }
        if (part->bootable) g_string_append(text, ", bootable");
        g_string_append_c(text, '\n');
    }
    return g_string_free(text, FALSE);
}

/* Use coarse, localized display sizes while keeping sector arithmetic exact. */
char *format_size(guint64 bytes)
{
    if (bytes>=G_GUINT64_CONSTANT(1099511627776)) return g_strdup_printf("%.1f TiB", bytes/1099511627776.0);
    if (bytes>=1073741824) return g_strdup_printf("%.1f GiB", bytes/1073741824.0);
    return g_strdup_printf("%.1f MiB", bytes/1048576.0);
}

/* Summarize the actual diff so the review cannot drift from the write plan. */
char *plan_description(const Disk *disk)
{
    GString *text=g_string_new(NULL);
    if (disk->new_table) g_string_append_printf(text, "Create %s partition table. All existing partitions will be removed.\n", !strcmp(disk->label, "gpt")?"GPT":"MBR");
    if (disk->repair) g_string_append(text, "Repair GPT to use the full disk capacity.\n");
    for (guint i=0; i<disk->base->len; i++)
    {
        Partition *part=disk->base->pdata[i];
        Partition *retained=find_part(disk->parts, part->number);
        if (!disk->new_table && (!retained || retained->origin!=part->number)) g_string_append_printf(text, "Delete partition %u (%s).\n", part->number, *part->name?part->name:part->fs);
    }
    for (guint i=0; i<disk->parts->len; i++)
    {
        Partition *part=disk->parts->pdata[i], *old=(!disk->new_table && part->origin)?find_part(disk->base, part->origin):NULL;
        char *size=format_size(part->size*disk->sector);
        if (!old) g_string_append_printf(text, "Create partition %u: %s (%s).\n", part->number, *part->name?part->name:"Untitled", size);
        else if (part->size!=old->size) g_string_append_printf(text, "Grow partition %u to %s.\n", part->number, size);
        if (old && strcmp(old->name, part->name)) g_string_append_printf(text, "Rename partition %u to %s.\n", part->number, part->name);
        if (*part->format) g_string_append_printf(text, "Format partition %u as %s. Existing files on this partition will be erased.\n", part->number, part->format);
        else if (part->grow) g_string_append_printf(text, "Grow the %s filesystem on partition %u.\n", part->fs, part->number);
        g_free(size);
    }
    return g_string_free(text, FALSE);
}

/* Choose an offline SVG from inventory facts, not a device's display name. */
const char *device_icon(const Disk *disk)
{
    if (g_str_has_prefix(disk->kind, "raid") || !strcmp(disk->kind, "md")) return "server";
    if (!strcmp(disk->kind, "lvm") || !strcmp(disk->kind, "crypt") || !strcmp(disk->kind, "loop") || !strcmp(disk->kind, "dm")) return "layer-group";
    if (!strcmp(disk->transport, "virtio") || g_str_has_prefix(disk->path, "/dev/vd")) return "layer-group";
    return !strcmp(disk->transport, "usb")?"usb":"hard-drive";
}

/* Newly drawn extents use MiB alignment without changing existing boundaries. */
guint64 align_up(guint64 value, guint64 grain)
{
    return ((value+grain-1)/grain)*grain;
}

/* Resolve filesystem adapters from the small, explicit supported feature set. */
Filesystem *find_filesystem(const char *id)
{
    for (guint i=0; i<filesystem_count; i++)
        if (!strcmp(filesystems[i].id, id)) return &filesystems[i];
    return NULL;
}

/* Resolve identifiers from sfdisk's current catalogue, including FAT EFI layouts. */
const char *filesystem_type(const Disk *disk, const char *fs)
{
    const char *name="Linux filesystem";
    if (!strcmp(disk->label, "dos"))
    {
        name="Linux";
        if (!strcmp(fs,"vfat")) name="W95 FAT32 (LBA)";
        else if (!strcmp(fs,"ntfs") || !strcmp(fs,"exfat")) name="HPFS/NTFS/exFAT";
    }
    else if (!strcmp(fs, "vfat") || !strcmp(fs, "ntfs") || !strcmp(fs, "exfat")) name="Microsoft basic data";
    for (guint i=0; i<disk->types->len; i++)
    {
        PartitionType *type=disk->types->pdata[i];
        if (!strcmp(type->name, name)) return type->id;
    }
    return NULL;
}
