#include "partition-manager.h"
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
    GtkWidget *window, *sidebar, *heading, *subtitle, *notice, *map, *rows, *details;
    GtkWidget *body, *content, *status, *spinner, *undo, *review, *create, *edit, *resize, *format, *remove;
    GtkWidget *header_device, *header_icon, *allocated, *available, *row_scroll, *status_icon;
    GtkSizeGroup *size_group;
    GPtrArray *disks, *history;
    guint active;
    gint selected;
    guint64 free_start;
    gboolean readonly, working, include_loop;
    char *only;
} App;
typedef struct { guint disk; Disk *before; } Undo;
typedef struct { guint64 start, size; gint number; } Item;
typedef struct { Item item; double x, width; } Tile;
typedef enum { SCAN, APPLY, CHECK } JobType;
typedef struct { GPtrArray *plans; char *backup, *only; GString *log; JobType type; ApplyOptions options; gboolean include_loop; } Job;
typedef enum { CREATE, ADJUST, GROW, FORMAT, RENAME } EditMode;
typedef struct
{
    App *app;
    GtkWidget *dialog, *area, *name, *fs, *capacity;
    EditMode mode;
    guint number;
    guint64 start, total, offset, size, minimum, saved_offset, saved_size;
    double anchor;
    int drag;
} Editor;

static void render(App *app, gboolean rebuild_rows);
static void start_job(App *app, JobType type, gboolean allow_busy);
static void action(GtkButton *button, gpointer data);
static void activate_selection(App *app);
static const double colors[][3]={{233/255.,215/255.,174/255.},{195/255.,213/255.,232/255.},{212/255.,204/255.,232/255.},{186/255.,219/255.,197/255.}};
static const double text_colors[][3]={{114/255.,89/255.,31/255.},{63/255.,92/255.,123/255.},{100/255.,82/255.,130/255.},{55/255.,99/255.,71/255.}};
static const double editor_inset=12;

/* Widgets use local CSS classes instead of altering the user's GTK theme. */
static void style(GtkWidget *widget, const char *name)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

/* Apply the current GTK foreground to the SVG's alpha mask, including disabled states. */
static gboolean icon_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    GdkPixbuf *pixbuf=data; GdkRGBA color;
    gtk_style_context_get_color(gtk_widget_get_style_context(widget),gtk_widget_get_state_flags(widget),&color);
    cairo_push_group(cr);
    gdk_cairo_set_source_pixbuf(cr,pixbuf,floor((gtk_widget_get_allocated_width(widget)-gdk_pixbuf_get_width(pixbuf))/2.),floor((gtk_widget_get_allocated_height(widget)-gdk_pixbuf_get_height(pixbuf))/2.));
    cairo_paint(cr); cairo_pattern_t *mask=cairo_pop_group(cr);
    gdk_cairo_set_source_rgba(cr,&color); cairo_mask(cr,mask); cairo_pattern_destroy(mask);
    return FALSE;
}

/* Reuse the selected Font Awesome SVGs offline through GResource. */
static GtkWidget *image(const char *name, int size)
{
    char *path=g_strdup_printf("/org/slax/partition-manager/icons/%s.svg", name);
    GdkPixbuf *pixbuf=gdk_pixbuf_new_from_resource_at_scale(path, size, size, TRUE, NULL);
    GtkWidget *widget=gtk_drawing_area_new(); gtk_widget_set_size_request(widget,size,size);
    gtk_widget_set_valign(widget,GTK_ALIGN_CENTER);
    g_signal_connect(widget,"draw",G_CALLBACK(icon_draw),pixbuf);
    g_object_set_data_full(G_OBJECT(widget),"pixbuf",pixbuf,g_object_unref); g_free(path);
    return widget;
}

/* Shared native buttons keep labels and compact toolbar icons aligned. */
static GtkWidget *button(App *app, const char *text, const char *icon, const char *command)
{
    GtkWidget *widget=gtk_button_new(), *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_halign(box,GTK_ALIGN_CENTER); gtk_widget_set_valign(box,GTK_ALIGN_CENTER);
    style(widget,"action-button");
    if (!text || !*text) style(widget,"icon-button");
    gtk_container_add(GTK_CONTAINER(widget), box);
    if (icon) gtk_box_pack_start(GTK_BOX(box), image(icon, 16), FALSE, FALSE, 0);
    if (text && *text)
    {
        GtkWidget *caption=gtk_label_new(text);
        gtk_box_pack_start(GTK_BOX(box), caption, FALSE, FALSE, 0); g_object_set_data(G_OBJECT(widget),"caption",caption);
    }
    g_object_set_data_full(G_OBJECT(widget), "action", g_strdup(command), g_free);
    g_signal_connect(widget, "clicked", G_CALLBACK(action), app);
    return widget;
}

/* Ellipsized cells keep the shared table columns independent of long Unicode names. */
static void ellipsize(GtkWidget *widget)
{
    gtk_label_set_ellipsize(GTK_LABEL(widget),PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(widget),1); gtk_label_set_max_width_chars(GTK_LABEL(widget),1);
}

/* Share proportional columns without turning their current allocation into a minimum width. */
static GtkWidget *table_columns(GtkWidget *name, GtkWidget *filesystem)
{
    GtkWidget *grid=gtk_grid_new(); gtk_grid_set_column_homogeneous(GTK_GRID(grid),TRUE);
    gtk_widget_set_hexpand(grid,TRUE); gtk_widget_set_margin_end(name,12);
    gtk_grid_attach(GTK_GRID(grid),name,0,0,2,1); gtk_grid_attach(GTK_GRID(grid),filesystem,2,0,1,1);
    return grid;
}

/* Use one native CSD header and footer treatment for the mockup's focused dialogs. */
static GtkWidget *make_dialog(App *app, const char *title, int width, const char *accept)
{
    GtkWidget *dialog=gtk_dialog_new_with_buttons(title,GTK_WINDOW(app->window),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"Cancel",GTK_RESPONSE_CANCEL,accept,GTK_RESPONSE_ACCEPT,NULL);
    style(dialog,"partitions-dialog"); gtk_window_set_default_size(GTK_WINDOW(dialog),width,-1);
    GtkWidget *header=gtk_header_bar_new(); gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header),TRUE);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header),":close");
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header),gtk_label_new(""));
    GtkWidget *heading=gtk_label_new(title); style(heading,"modal-title"); gtk_header_bar_pack_start(GTK_HEADER_BAR(header),heading);
    gtk_window_set_titlebar(GTK_WINDOW(dialog),header); gtk_window_set_title(GTK_WINDOW(dialog),title); gtk_widget_show_all(header);
    GtkWidget *content=gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(content),0); gtk_container_set_border_width(GTK_CONTAINER(content),0);
    GtkWidget *body=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); gtk_container_set_border_width(GTK_CONTAINER(body),23);
    gtk_box_pack_start(GTK_BOX(content),body,TRUE,TRUE,0); g_object_set_data(G_OBJECT(dialog),"body",body);
    GtkWidget *footer=gtk_dialog_get_action_area(GTK_DIALOG(dialog)); style(footer,"modal-footer");
    gtk_button_box_set_child_non_homogeneous(GTK_BUTTON_BOX(footer),gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL),TRUE);
    gtk_button_box_set_child_non_homogeneous(GTK_BUTTON_BOX(footer),gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT),TRUE);
    GtkWidget *apply=gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT); style(apply,"suggested-action");
    return dialog;
}

/* Markup is escaped before any disk or user-provided text is inserted. */
static GtkWidget *label(const char *text, const char *class)
{
    GtkWidget *widget=gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (class) style(widget, class);
    return widget;
}

/* Destroy only a view's children; model ownership stays with the application. */
static void clear(GtkWidget *container)
{
    GList *children=gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *item=children; item; item=item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
}

/* Errors and command logs remain selectable in a bounded native dialog. */
static void message(App *app, const char *title, const char *text, const char *details)
{
    GtkWidget *dialog=make_dialog(app,title,details && *details?680:500,"Close"), *body=g_object_get_data(G_OBJECT(dialog),"body");
    GtkWidget *cancel=gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL); gtk_widget_hide(cancel); gtk_widget_set_no_show_all(cancel,TRUE);
    GtkWidget *description=label(text,NULL); gtk_label_set_line_wrap(GTK_LABEL(description),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(description),70); gtk_label_set_selectable(GTK_LABEL(description),TRUE);
    gtk_box_pack_start(GTK_BOX(body),description,FALSE,FALSE,0);
    if (details && *details)
    {
        GtkWidget *scroll=gtk_scrolled_window_new(NULL, NULL), *view=gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view),FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), details, -1);
        gtk_container_add(GTK_CONTAINER(scroll), view);
        gtk_widget_set_size_request(scroll, 580, 210); style(scroll,"log-view");
        gtk_box_pack_start(GTK_BOX(body),scroll,TRUE,TRUE,16);
    }
    gtk_widget_show_all(dialog); gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
}

/* Resolve the active selection without keeping dangling pointers across Undo. */
static Partition *selected_part(App *app)
{
    return app->disks->len && app->selected>=0?find_part(((Disk *)app->disks->pdata[app->active])->parts, app->selected):NULL;
}

/* Order real partitions and derived free intervals for the list and allocation map. */
static gint compare_item(gconstpointer a, gconstpointer b)
{
    const Item *left=a, *right=b;
    return (left->start>right->start)-(left->start<right->start);
}

/* Both views share these derived intervals, including tiny alignment gaps. */
static GArray *items(Disk *disk)
{
    GArray *list=g_array_new(FALSE, FALSE, sizeof(Item)), *free=free_regions(disk);
    for (guint i=0; i<disk->parts->len; i++)
    {
        Partition *part=disk->parts->pdata[i]; Item item={part->start, part->size, part->number};
        g_array_append_val(list, item);
    }
    for (guint i=0; i<free->len; i++)
    {
        Region region=g_array_index(free, Region, i); Item item={region.start, region.size, -1};
        g_array_append_val(list, item);
    }
    if (!strcmp(disk->label,"gpt") && disk->last<capacity_last(disk))
    {
        Item blocked={disk->last+1,capacity_last(disk)-disk->last,-2}; g_array_append_val(list,blocked);
    }
    g_array_unref(free); g_array_sort(list, compare_item);
    return list;
}

/* Choose the free interval at the remembered address or the first valid row. */
static Item selection(App *app)
{
    Disk *disk=app->disks->pdata[app->active]; GArray *list=items(disk);
    Item chosen={0,0,-1};
    if (list->len) chosen=g_array_index(list, Item, 0);
    for (guint i=0; i<list->len; i++)
    {
        Item item=g_array_index(list, Item, i);
        if (app->selected<0 && !app->free_start)
        {
            if (item.number==-2) chosen=item;
            if (item.number==-1 && item.size*disk->sector>=4194304) { chosen=item; break; }
        }
        if (item.number==app->selected && (item.number>=0 || item.start==app->free_start)) { chosen=item; break; }
    }
    app->selected=chosen.number; app->free_start=chosen.start;
    g_array_unref(list); return chosen;
}

/* Supply a familiar name for unnamed, MBR and whole-device filesystems. */
static char *part_title(const Partition *part)
{
    if (*part->name) return g_strdup(part->name);
    if (*part->fslabel) return g_strdup(part->fslabel);
    return part->number?g_strdup_printf("Partition %u", part->number):g_strdup("Whole-device filesystem");
}

/* Use the expected kernel partition naming convention only for presentation. */
static char *part_path(const Disk *disk, const Partition *part)
{
    return part->number?g_strdup_printf("%s%s%u", disk->path, g_ascii_isdigit(disk->path[strlen(disk->path)-1])?"p":"", part->number):g_strdup(disk->path);
}

/* Keep filesystem names consistent between pending formatting and existing data. */
static const char *fs_name(const Partition *part)
{
    const char *id=*part->format?part->format:part->fs;
    Filesystem *fs=find_filesystem(id);
    return fs?fs->name:*id?id:"Unformatted";
}

/* Disable unsupported growth while keeping retry of an incomplete filesystem grow. */
static gboolean can_grow(Disk *disk, Partition *part)
{
    if (!part || !part->origin || part->readonly || !part->number || *part->format) return FALSE;
    Filesystem *fs=find_filesystem(g_str_has_prefix(part->fs, "ext")?"ext4":part->fs);
    gboolean room=grow_limit(disk, part)>part->size || (part->fsbytes && part->fsbytes<part->size*disk->sector);
    if (!*part->fs) return room;
    return fs && fs->grow_available && room;
}

/* Snapshot the active disk just before a user changes its desired layout. */
static void remember(App *app)
{
    Undo *undo=g_new0(Undo, 1); undo->disk=app->active;
    undo->before=disk_copy(app->disks->pdata[app->active]);
    g_ptr_array_add(app->history, undo);
}

/* Release consumed or obsolete history entries. */
static void undo_free(gpointer data)
{
    Undo *undo=data;
    if (undo->before) disk_free(undo->before);
    g_free(undo);
}

/* Clip text to its allotted native drawing area without breaking Unicode. */
static void draw_text(GtkWidget *widget, cairo_t *cr, const char *text, double x, double y, double width, gboolean small)
{
    PangoLayout *layout=gtk_widget_create_pango_layout(widget, text);
    pango_layout_set_width(layout, MAX(0, width)*PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    PangoFontDescription *font=pango_font_description_from_string("DejaVu Sans");
    pango_font_description_set_absolute_size(font,13*PANGO_SCALE);
    pango_font_description_set_weight(font,small?PANGO_WEIGHT_NORMAL:PANGO_WEIGHT_MEDIUM);
    pango_layout_set_font_description(layout, font);
    cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font); g_object_unref(layout);
}

/* Rounded paths are shared by the disk map, draft partition and resize handles. */
static void rounded(cairo_t *cr, double x, double y, double width, double height, double radius)
{
    radius=MIN(radius,MIN(width,height)/2);
    cairo_new_sub_path(cr); cairo_arc(cr,x+width-radius,y+radius,radius,-G_PI/2,0);
    cairo_arc(cr,x+width-radius,y+height-radius,radius,0,G_PI/2);
    cairo_arc(cr,x+radius,y+height-radius,radius,G_PI/2,G_PI);
    cairo_arc(cr,x+radius,y+radius,radius,G_PI,3*G_PI/2); cairo_close_path(cr);
}

/* A clipped diagonal pattern matches the mockup's unallocated areas. */
static void hatch(cairo_t *cr, double x, double y, double width, double height)
{
    cairo_save(cr); cairo_clip(cr); cairo_set_source_rgb(cr,245/255.,247/255.,241/255.); cairo_paint(cr);
    cairo_set_source_rgb(cr,237/255.,241/255.,231/255.); cairo_set_line_width(cr,2);
    for (double offset=-height; offset<width; offset+=7)
    { cairo_move_to(cr,x+offset,y+height); cairo_line_to(cr,x+offset+height,y); }
    cairo_stroke(cr); cairo_restore(cr);
}

/* Tiny partitions keep a selectable minimum width while the rest stays proportional. */
static GArray *tiles(App *app, double width)
{
    Disk *disk=app->disks->pdata[app->active]; GArray *list=items(disk), *result=g_array_new(FALSE, FALSE, sizeof(Tile));
    double available=MAX(1, width-12-4*(MAX(1,list->len)-1)), weights=0;
    for (guint i=0; i<list->len; i++) weights+=MAX(22, ((Item *)list->data)[i].size/(double)(disk->bytes/disk->sector)*available);
    double x=6;
    for (guint i=0; i<list->len; i++)
    {
        Item item=g_array_index(list, Item, i);
        double size=MAX(22, item.size/(double)(disk->bytes/disk->sector)*available)/MAX(1, weights)*available;
        Tile tile={item,x,size}; g_array_append_val(result, tile); x+=size+4;
    }
    g_array_unref(list); return result;
}

/* Draw the selected disk's allocation map using Cairo and Pango. */
static gboolean map_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    App *app=data;
    if (!app->disks->len) return FALSE;
    Disk *disk=app->disks->pdata[app->active];
    double width=gtk_widget_get_allocated_width(widget);
    rounded(cr,.5,.5,width-1,67,6); cairo_set_source_rgb(cr,250/255.,251/255.,248/255.); cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr,220/255.,225/255.,220/255.); cairo_set_line_width(cr,1); cairo_stroke(cr);
    GArray *list=tiles(app, gtk_widget_get_allocated_width(widget));
    for (guint i=0; i<list->len; i++)
    {
        Tile tile=g_array_index(list, Tile, i); Partition *part=tile.item.number>=0?find_part(disk->parts, tile.item.number):NULL;
        guint index=part?MAX(1,part->number)-1:0;
        const double *color=colors[index%4], *foreground=text_colors[index%4];
        rounded(cr,tile.x,6,tile.width,56,3);
        if (part) { cairo_set_source_rgb(cr,color[0],color[1],color[2]); cairo_fill(cr); }
        else hatch(cr,tile.x,6,tile.width,56);
        gboolean selected=app->selected==tile.item.number && (part || app->free_start==tile.item.start);
        cairo_set_line_width(cr,selected?2:1);
        if (selected) cairo_set_source_rgb(cr,75/255.,131/255.,93/255.);
        else cairo_set_source_rgba(cr,0,0,0,.05);
        rounded(cr,tile.x+.5,6.5,tile.width-1,55,3); cairo_stroke(cr);
        char *title=part?part_title(part):g_strdup(tile.item.number==-2?"Needs GPT repair":"Unallocated");
        char *size=format_size(tile.item.size*disk->sector);
        if (part) cairo_set_source_rgb(cr,foreground[0],foreground[1],foreground[2]); else cairo_set_source_rgb(cr,101/255.,115/255.,91/255.);
        if (tile.width>40) draw_text(widget,cr,title,tile.x+10,15,tile.width-20,FALSE);
        else if (part)
        {
            char *number=g_strdup_printf("%u",part->number); draw_text(widget,cr,number,tile.x+6,25,tile.width-8,FALSE); g_free(number);
        }
        if (tile.width>65)
        {
            if (part) cairo_set_source_rgba(cr,foreground[0],foreground[1],foreground[2],.7);
            draw_text(widget,cr,size,tile.x+10,34,tile.width-20,TRUE);
        }
        g_free(title); g_free(size);
    }
    g_array_unref(list); return FALSE;
}

/* Select the map interval before dispatching its default double-click action. */
static gboolean map_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    App *app=data;
    if (event->button!=1 || !app->disks->len) return FALSE;
    GArray *list=tiles(app, gtk_widget_get_allocated_width(widget)); gboolean hit=FALSE;
    for (guint i=0; i<list->len; i++)
    {
        Tile tile=g_array_index(list, Tile, i);
        if (event->x>=tile.x && event->x<=tile.x+tile.width)
        { app->selected=tile.item.number; app->free_start=tile.item.start; render(app,FALSE); hit=TRUE; break; }
    }
    g_array_unref(list);
    if (hit && event->type==GDK_2BUTTON_PRESS) activate_selection(app);
    return hit;
}

/* Native list selection and the map always point to the same model interval. */
static void row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box; App *app=data;
    if (!row) return;
    Item *item=g_object_get_data(G_OBJECT(row), "item");
    if (app->selected==item->number && app->free_start==item->start) return;
    app->selected=item->number; app->free_start=item->start; render(app,FALSE);
}

/* Rows and map cells share the same context-sensitive activation. */
static void row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    App *app=data; row_selected(box,row,app); activate_selection(app);
}

/* Switch devices without discarding pending operations on the previous disk. */
static void disk_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box; App *app=data;
    if (!row) return;
    guint index=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "index"));
    if (index==app->active) return;
    app->active=index; app->selected=-1; app->free_start=0; render(app,TRUE);
}

/* Build a compact sidebar row with its inventory-derived device icon. */
static void render_sidebar(App *app)
{
    g_signal_handlers_block_by_func(app->sidebar, disk_selected, app); clear(app->sidebar);
    for (guint i=0; i<app->disks->len; i++)
    {
        Disk *disk=app->disks->pdata[i]; GtkWidget *row=gtk_list_box_row_new();
        style(row,"device-row");
        GtkWidget *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10), *text=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
        GtkWidget *symbol=image(device_icon(disk),21); style(symbol,"device-symbol");
        gtk_widget_set_size_request(symbol,24,24); gtk_widget_set_valign(symbol,GTK_ALIGN_START); gtk_widget_set_margin_top(symbol,2);
        gtk_box_pack_start(GTK_BOX(box),symbol,FALSE,FALSE,0);
        GtkWidget *name=label(disk->title,"device-name"); ellipsize(name);
        gtk_box_pack_start(GTK_BOX(text), name, FALSE, FALSE, 0);
        char *size=format_size(disk->bytes), *detail=g_strdup_printf("%s · %s", disk->path, size);
        GtkWidget *sub=label(detail,"device-subtitle"); ellipsize(sub);
        gtk_box_pack_start(GTK_BOX(text), sub, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0); gtk_container_add(GTK_CONTAINER(row), box);
        g_object_set_data(G_OBJECT(row), "index", GUINT_TO_POINTER(i)); gtk_container_add(GTK_CONTAINER(app->sidebar), row);
        if (i==app->active) gtk_list_box_select_row(GTK_LIST_BOX(app->sidebar), GTK_LIST_BOX_ROW(row));
        g_free(size); g_free(detail);
    }
    gtk_widget_show_all(app->sidebar); g_signal_handlers_unblock_by_func(app->sidebar, disk_selected, app);
}

/* Update the Workspace while all state remains in the model and Undo snapshots. */
static void render(App *app, gboolean rebuild_rows)
{
    if (!app->disks->len) return;
    Disk *disk=app->disks->pdata[app->active]; Item chosen=selection(app); Partition *part=selected_part(app);
    g_signal_handlers_block_by_func(app->sidebar,disk_selected,app);
    gtk_list_box_select_row(GTK_LIST_BOX(app->sidebar),gtk_list_box_get_row_at_index(GTK_LIST_BOX(app->sidebar),app->active));
    g_signal_handlers_unblock_by_func(app->sidebar,disk_selected,app);
    char *title=g_strdup_printf("%s · %s — Partition Manager", disk->title, disk->path);
    gtk_window_set_title(GTK_WINDOW(app->window), title); g_free(title);
    gtk_label_set_text(GTK_LABEL(app->heading), disk->title);
    char *header_title=g_strdup_printf("%s · %s",disk->title,disk->path);
    gtk_label_set_text(GTK_LABEL(app->header_device),header_title); g_free(header_title);
    clear(app->header_icon); gtk_container_add(GTK_CONTAINER(app->header_icon),image(device_icon(disk),16)); gtk_widget_show_all(app->header_icon);
    char *size=format_size(disk->bytes), *subtitle=g_strdup_printf("%s   ·   %s   ·   %s", disk->path, size, !strcmp(disk->label,"dos")?"MBR":*disk->label?"GPT":"No partition table");
    gtk_label_set_text(GTK_LABEL(app->subtitle), subtitle); g_free(size); g_free(subtitle);
    clear(app->notice);
    if (!strcmp(disk->label, "gpt") && disk->last<capacity_last(disk))
    {
        GtkWidget *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *note=label("This image's GPT describes a smaller disk. Repair it to use the remaining space.", NULL);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_box_pack_start(GTK_BOX(box), note, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), button(app,"Repair layout",NULL,"repair"), FALSE,FALSE,0);
        gtk_container_add(GTK_CONTAINER(app->notice), box);
    }
    if (disk->unsupported || disk->readonly)
    {
        GtkWidget *note=label(disk->readonly?"This device is read-only.":disk->warning,"muted");
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE); gtk_container_add(GTK_CONTAINER(app->notice), note);
    }
    gtk_widget_show_all(app->notice);
    gboolean editable=!disk->readonly && !disk->unsupported && !app->working;
    gtk_widget_set_sensitive(app->create, editable && chosen.number==-1 && *disk->label && chosen.size*disk->sector>=4194304);
    GtkStyleContext *create_style=gtk_widget_get_style_context(app->create);
    if (editable && !part) gtk_style_context_add_class(create_style,"suggested-action"); else gtk_style_context_remove_class(create_style,"suggested-action");
    gtk_widget_set_sensitive(app->edit, editable && part && part->number && (!part->origin || !strcmp(disk->label,"gpt")));
    gboolean planned=part && part->number && !part->origin;
    gtk_widget_set_sensitive(app->resize, editable && (planned || can_grow(disk,part)));
    gtk_widget_set_sensitive(app->format, editable && part && part->number && !part->readonly);
    gtk_widget_set_sensitive(app->remove, editable && part && part->number);
    g_signal_handlers_block_by_func(app->rows, row_selected, app);
    if (rebuild_rows) clear(app->rows);
    GArray *list=items(disk);
    for (guint i=0; i<list->len; i++)
    {
        Item item=g_array_index(list,Item,i); Partition *entry=item.number>=0?find_part(disk->parts,item.number):NULL;
        if (!rebuild_rows)
        {
            if (item.number==chosen.number && item.start==chosen.start)
                gtk_list_box_select_row(GTK_LIST_BOX(app->rows),gtk_list_box_get_row_at_index(GTK_LIST_BOX(app->rows),i));
            continue;
        }
        GtkWidget *row=gtk_list_box_row_new(), *box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12), *text=gtk_box_new(GTK_ORIENTATION_VERTICAL,2);
        style(row,"partition-row"); gtk_widget_set_size_request(row,-1,57);
        char *name=entry?part_title(entry):g_strdup(item.number==-2?"Unaddressed space":"Unallocated space");
        char *path=entry?part_path(disk,entry):g_strdup(item.number==-2?"Repair GPT to address this space":"Available for a new partition");
        GtkWidget *heading=label(name,"row-name"), *sub=label(path,"row-subtitle"); ellipsize(heading); ellipsize(sub);
        gtk_box_pack_start(GTK_BOX(text),heading,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(text),sub,FALSE,FALSE,0);
        GtkWidget *name_cell=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,9), *dot=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
        style(dot,"dot");
        char *color=entry?g_strdup_printf("color%u",(MAX(1,entry->number)-1)%4):g_strdup("free-dot"); style(dot,color); g_free(color);
        gtk_widget_set_size_request(dot,10,10); gtk_widget_set_valign(dot,GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(name_cell),dot,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(name_cell),text,TRUE,TRUE,0);
        GtkWidget *fs=label(entry?fs_name(entry):item.number==-2?"Needs repair":"Unpartitioned","row-type"); ellipsize(fs);
        gtk_box_pack_start(GTK_BOX(box),table_columns(name_cell,fs),TRUE,TRUE,0);
        char *capacity=format_size(item.size*disk->sector);
        GtkWidget *capacity_label=label(capacity,"row-size"); gtk_size_group_add_widget(app->size_group,capacity_label); gtk_label_set_xalign(GTK_LABEL(capacity_label),1);
        gtk_box_pack_start(GTK_BOX(box),capacity_label,FALSE,FALSE,0); gtk_container_add(GTK_CONTAINER(row),box);
        Item *stored=g_memdup2(&item,sizeof item); g_object_set_data_full(G_OBJECT(row),"item",stored,g_free);
        gtk_container_add(GTK_CONTAINER(app->rows),row);
        if (item.number==chosen.number && item.start==chosen.start) gtk_list_box_select_row(GTK_LIST_BOX(app->rows),GTK_LIST_BOX_ROW(row));
        g_free(name); g_free(path); g_free(capacity);
    }
    g_array_unref(list); gtk_widget_show_all(app->rows); g_signal_handlers_unblock_by_func(app->rows,row_selected,app);
    guint64 total_free=0,total_allocated=0; GArray *regions=free_regions(disk);
    for (guint i=0;i<regions->len;i++) total_free+=g_array_index(regions,Region,i).size*disk->sector;
    for (guint i=0;i<disk->parts->len;i++) total_allocated+=((Partition *)disk->parts->pdata[i])->size*disk->sector;
    g_array_unref(regions);
    char *used=format_size(total_allocated), *left=format_size(total_free), *caption=g_strdup_printf("%u %s  ·  %s allocated",disk->parts->len,disk->parts->len==1?"partition":"partitions",used), *available=g_strdup_printf("%s unallocated",left);
    gboolean needs_repair=!strcmp(disk->label,"gpt") && disk->last<capacity_last(disk);
    gtk_label_set_text(GTK_LABEL(app->allocated),caption); gtk_label_set_text(GTK_LABEL(app->available),needs_repair?"GPT repair needed":available);
    g_free(used); g_free(left); g_free(caption); g_free(available);
    clear(app->details);
    char *name=part?part_title(part):g_strdup(chosen.number==-2?"GPT needs repair":"Unallocated space"), *capacity=format_size(chosen.size*disk->sector);
    gtk_box_pack_start(GTK_BOX(app->details),label(name,"detail-title"),FALSE,FALSE,0);
    GtkWidget *metric=label(capacity,"capacity"); gtk_widget_set_margin_top(metric,13); gtk_widget_set_margin_bottom(metric,20);
    gtk_box_pack_start(GTK_BOX(app->details),metric,FALSE,FALSE,0);
    const char *hint=part?"Existing partitions stay in place.":"Create a partition here to make room for your files.";
    if (chosen.number==-2) hint="Repair the GPT layout to use the rest of this disk.";
    GtkWidget *note=label(hint,"detail-hint"); gtk_label_set_line_wrap(GTK_LABEL(note),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(note),26);
    gtk_widget_set_margin_bottom(note,17); gtk_box_pack_start(GTK_BOX(app->details),note,FALSE,FALSE,0);
    GtkWidget *device=gtk_box_new(GTK_ORIENTATION_VERTICAL,3); style(device,"detail-field");
    gtk_box_pack_start(GTK_BOX(device),label("Device","detail-key"),FALSE,FALSE,0);
    char *device_path=part?part_path(disk,part):g_strdup(disk->path); GtkWidget *device_value=label(device_path,NULL); ellipsize(device_value); g_free(device_path);
    gtk_box_pack_start(GTK_BOX(device),device_value,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(app->details),device,FALSE,FALSE,0);
    if (part)
    {
        GtkWidget *filesystem=gtk_box_new(GTK_ORIENTATION_VERTICAL,3), *status=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
        style(filesystem,"detail-field"); style(status,"detail-field");
        gtk_box_pack_start(GTK_BOX(filesystem),label("Filesystem","detail-key"),FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(filesystem),label(fs_name(part),NULL),FALSE,FALSE,0);
        const char *state="Not mounted";
        if (part->busy) state="In use";
        if (*part->mount) state="Mounted";
        if (part->number && !part->origin) state="Will be created";
        gtk_box_pack_start(GTK_BOX(status),label("Status","detail-key"),FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(status),label(state,NULL),FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(app->details),filesystem,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(app->details),status,FALSE,FALSE,0);
        if (part->fsbytes && part->fsbytes<part->size*disk->sector)
        {
            char *fs_size=format_size(part->fsbytes), *text=g_strdup_printf("Filesystem: %s",fs_size);
            gtk_box_pack_start(GTK_BOX(app->details),label(text,"muted"),FALSE,FALSE,6); g_free(fs_size); g_free(text);
        }
        if (!part->origin && part->number) gtk_box_pack_start(GTK_BOX(app->details),button(app,"Resize partition","up-right-and-down-left-from-center","resize"),FALSE,FALSE,12);
    }
    gtk_widget_show_all(app->details); g_free(name); g_free(capacity);
    guint pending=0;
    for (guint i=0; i<app->disks->len; i++) if (disk_changed(app->disks->pdata[i])) pending++;
    char *status=pending?g_strdup_printf("Pending changes on %u %s",pending,pending==1?"device":"devices"):g_strdup("No pending changes. Your disks are unchanged.");
    gtk_label_set_text(GTK_LABEL(app->status),app->readonly?"Read-only session. Run as administrator to apply changes.":status); g_free(status);
    gtk_widget_set_sensitive(app->undo,app->history->len && !app->working);
    gtk_widget_set_sensitive(app->review,pending && !app->working);
    clear(app->status_icon); gtk_container_add(GTK_CONTAINER(app->status_icon),image(pending?"pen-to-square":"check",17)); gtk_widget_show_all(app->status_icon);
    gtk_widget_hide(app->spinner);
    gtk_widget_queue_draw(app->map);
}

/* Make drag feedback readable without offering manual sector or size entry. */
static void editor_update(Editor *editor)
{
    Disk *disk=editor->app->disks->pdata[editor->app->active];
    char *size=format_size(editor->size*disk->sector);
    gtk_label_set_text(GTK_LABEL(editor->capacity),size); g_free(size);
    gtk_widget_queue_draw(editor->area);
}

/* Keep complete handles and minimum-width selections inside the drawing area. */
static void editor_bounds(GtkWidget *widget, Editor *editor, double *left, double *right)
{
    double width=gtk_widget_get_allocated_width(widget)-2*editor_inset;
    double size=MAX(16,editor->size/(double)editor->total*width);
    *left=editor_inset+MIN(editor->offset/(double)editor->total*width,width-size);
    *right=*left+size;
}

/* Render one interval with two resize handles and a movable centre for new data. */
static gboolean editor_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Editor *editor=data; double width=gtk_widget_get_allocated_width(widget)-2*editor_inset, x,right;
    editor_bounds(widget,editor,&x,&right); double size=right-x;
    rounded(cr,editor_inset,.5,width,103,7); hatch(cr,editor_inset,0,width,104);
    rounded(cr,editor_inset,.5,width,103,7); cairo_set_source_rgb(cr,205/255.,217/255.,194/255.); cairo_set_line_width(cr,1); cairo_stroke(cr);
    rounded(cr,x,.5,size,103,6); cairo_set_source_rgb(cr,184/255.,215/255.,188/255.); cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr,103/255.,149/255.,109/255.); cairo_stroke(cr);
    for (int edge=0;edge<2;edge++)
    {
        if (!edge && editor->mode==GROW) continue;
        double handle=x+(edge?size:0)-9;
        rounded(cr,handle,27,18,48,5); cairo_set_source_rgb(cr,248/255.,251/255.,244/255.); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr,113/255.,153/255.,119/255.); cairo_stroke(cr);
        cairo_move_to(cr,handle+7,44); cairo_line_to(cr,handle+7,58); cairo_move_to(cr,handle+11,44); cairo_line_to(cr,handle+11,58); cairo_stroke(cr);
    }
    if (size>50)
    {
        GdkPixbuf *glyph=g_object_get_data(G_OBJECT(widget),"glyph");
        cairo_push_group(cr); gdk_cairo_set_source_pixbuf(cr,glyph,x+(size-17)/2,29); cairo_paint(cr); cairo_pattern_t *mask=cairo_pop_group(cr);
        cairo_set_source_rgb(cr,60/255.,111/255.,67/255.); cairo_mask(cr,mask); cairo_pattern_destroy(mask);
        Partition *part=selected_part(editor->app); const char *name=part && *part->name?part->name:"New partition";
        PangoLayout *layout=gtk_widget_create_pango_layout(widget,name); pango_layout_set_width(layout,(size-32)*PANGO_SCALE); pango_layout_set_alignment(layout,PANGO_ALIGN_CENTER); pango_layout_set_ellipsize(layout,PANGO_ELLIPSIZE_END);
        cairo_move_to(cr,x+16,54); pango_cairo_show_layout(cr,layout); g_object_unref(layout);
    }
    return FALSE;
}

/* Dragging the left edge is intentionally unavailable for existing filesystems. */
static gboolean editor_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Editor *editor=data;
    if (event->button!=1) return FALSE;
    double left,right; editor_bounds(widget,editor,&left,&right);
    editor->drag=0;
    if (fabs(event->x-right)<13) editor->drag=2;
    else if (editor->mode!=GROW && fabs(event->x-left)<13) editor->drag=1;
    else if (editor->mode!=GROW && event->x>left && event->x<right) editor->drag=3;
    editor->anchor=event->x; editor->saved_offset=editor->offset; editor->saved_size=editor->size;
    return editor->drag!=0;
}

/* Snap drawn boundaries to MiB intervals while preserving each allowed extent. */
static gboolean editor_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    Editor *editor=data; double width=gtk_widget_get_allocated_width(widget)-2*editor_inset;
    Disk *disk=editor->app->disks->pdata[editor->app->active]; guint64 grain=1048576/disk->sector;
    if (editor->drag && (event->state&GDK_BUTTON1_MASK))
    {
        gint64 delta=llround((event->x-editor->anchor)/width*editor->total/grain)*grain;
        gint64 offset=editor->saved_offset, size=editor->saved_size;
        if (editor->drag==1) { offset=CLAMP(offset+delta,0,offset+size-(gint64)editor->minimum); size+=editor->saved_offset-offset; }
        if (editor->drag==2) size=CLAMP(size+delta,(gint64)editor->minimum,(gint64)editor->total-offset);
        if (editor->drag==3) offset=CLAMP(offset+delta,0,(gint64)editor->total-size);
        editor->offset=offset; editor->size=size; editor_update(editor);
    }
    double left,right; editor_bounds(widget,editor,&left,&right);
    const char *cursor="default";
    if (fabs(event->x-right)<13 || (editor->mode!=GROW && fabs(event->x-left)<13)) cursor="ew-resize";
    else if (editor->mode!=GROW && event->x>left && event->x<right) cursor="grab";
    GdkCursor *shape=gdk_cursor_new_from_name(gtk_widget_get_display(widget),cursor);
    gdk_window_set_cursor(gtk_widget_get_window(widget),shape); g_clear_object(&shape);
    return TRUE;
}

/* Release the implicit pointer drag when its initiating button is released. */
static gboolean editor_release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; (void)event; ((Editor *)data)->drag=0; return TRUE;
}

/* Keyboard users can grow or shrink the selection without numeric input fields. */
static gboolean editor_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget; Editor *editor=data;
    guint64 step=MAX((guint64)1,editor->total/100);
    if (event->keyval==GDK_KEY_Right) editor->size=MIN(editor->size+step,editor->total-editor->offset);
    else if (event->keyval==GDK_KEY_Left) editor->size=MAX(editor->minimum,editor->size-MIN(editor->size,step));
    else return FALSE;
    editor_update(editor); return TRUE;
}

/* Fill the interval only after the user's explicit Use all space action. */
static void editor_all(GtkButton *button, gpointer data)
{
    (void)button; Editor *editor=data; editor->offset=0; editor->size=editor->total; editor_update(editor);
}

/* Offer only the supported formatters actually present on this system. */
static GtkWidget *filesystem_combo(const char *current, gboolean empty)
{
    GtkWidget *combo=gtk_combo_box_text_new();
    if (empty) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"","Leave unformatted");
    for (guint i=0; i<filesystem_count; i++)
        if (filesystems[i].format_available) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),filesystems[i].id,filesystems[i].name);
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),current)) gtk_combo_box_set_active(GTK_COMBO_BOX(combo),0);
    return combo;
}

/* The focused editor keeps geometry visual and fields limited to name and filesystem. */
static void edit_partition(App *app, EditMode mode)
{
    Disk *disk=app->disks->pdata[app->active]; Partition *part=selected_part(app); Item item=selection(app);
    Editor editor={.app=app,.mode=mode,.number=part?part->number:0,.start=item.start,.total=item.size,.size=item.size,.minimum=MAX((guint64)1,4194304/disk->sector)};
    if (mode==GROW) { editor.total=grow_limit(disk,part); editor.minimum=part->size; }
    if (mode==ADJUST)
    {
        Disk *copy=disk_copy(disk);
        for (guint i=0; i<copy->parts->len; i++) if (((Partition *)copy->parts->pdata[i])->number==part->number) { g_ptr_array_remove_index(copy->parts,i); break; }
        GArray *free=free_regions(copy);
        for (guint i=0; i<free->len; i++)
        {
            Region region=g_array_index(free,Region,i);
            if (region.start<=part->start && region.start+region.size>=part->start+part->size)
            { editor.start=region.start; editor.total=region.size; editor.offset=part->start-region.start; }
        }
        g_array_unref(free); disk_free(copy);
    }
    if (mode==CREATE)
    {
        editor.start=align_up(item.start,1048576/disk->sector);
        editor.total=item.start+item.size-editor.start; editor.size=MIN(editor.total,G_GUINT64_CONSTANT(2147483648)/disk->sector);
    }
    const char *titles[]={"New partition","Resize new partition","Resize partition","Format partition","Rename partition"};
    char *target=part?part_path(disk,part):g_strdup(""), *dialog_title=mode==GROW?g_strdup_printf("Resize %s",*part->name?part->name:target):g_strdup(titles[mode]);
    editor.dialog=make_dialog(app,dialog_title,mode<=GROW?640:500,mode==FORMAT?"Add format to plan":"Add to plan"); g_free(dialog_title);
    GtkWidget *content=g_object_get_data(G_OBJECT(editor.dialog),"body");
    if (mode==FORMAT)
    {
        GtkWidget *warning=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12), *text=gtk_box_new(GTK_ORIENTATION_VERTICAL,4); style(warning,"format-warning");
        GtkWidget *symbol=image("triangle-exclamation",17); gtk_widget_set_valign(symbol,GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(warning),symbol,FALSE,FALSE,0);
        char *description=g_strdup_printf("All files on %s will be erased.",target);
        GtkWidget *title=label(description,NULL); gtk_label_set_line_wrap(GTK_LABEL(title),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(title),42); g_free(description);
        gtk_box_pack_start(GTK_BOX(text),title,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(text),label("The other partitions will be kept.","format-note"),FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(warning),text,TRUE,TRUE,0); gtk_box_pack_start(GTK_BOX(content),warning,FALSE,FALSE,0);
    }
    g_free(target);
    if (mode<=GROW)
    {
        GtkWidget *summary=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,20); style(summary,"placement-summary");
        gtk_box_pack_start(GTK_BOX(summary),label(mode==GROW?"Grow into the available space":"Choose your partition size",NULL),TRUE,TRUE,0);
        editor.capacity=label("","capacity"); style(editor.capacity,"editor-capacity"); gtk_box_pack_end(GTK_BOX(summary),editor.capacity,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(content),summary,FALSE,FALSE,0);
        editor.area=gtk_drawing_area_new(); gtk_widget_set_size_request(editor.area,580,104); gtk_widget_set_can_focus(editor.area,TRUE);
        const char *glyph=mode==GROW?"/org/slax/partition-manager/icons/up-right-and-down-left-from-center.svg":"/org/slax/partition-manager/icons/hard-drive.svg";
        g_object_set_data_full(G_OBJECT(editor.area),"glyph",gdk_pixbuf_new_from_resource_at_scale(glyph,17,17,TRUE,NULL),g_object_unref);
        gtk_widget_add_events(editor.area,GDK_BUTTON_PRESS_MASK|GDK_BUTTON_RELEASE_MASK|GDK_POINTER_MOTION_MASK|GDK_KEY_PRESS_MASK);
        g_signal_connect(editor.area,"draw",G_CALLBACK(editor_draw),&editor);
        g_signal_connect(editor.area,"button-press-event",G_CALLBACK(editor_press),&editor);
        g_signal_connect(editor.area,"motion-notify-event",G_CALLBACK(editor_motion),&editor);
        g_signal_connect(editor.area,"button-release-event",G_CALLBACK(editor_release),&editor);
        g_signal_connect(editor.area,"key-press-event",G_CALLBACK(editor_key),&editor);
        gtk_box_pack_start(GTK_BOX(content),editor.area,FALSE,FALSE,0);
        GtkWidget *legend=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10); style(legend,"placement-legend"); gtk_box_pack_start(GTK_BOX(legend),label("Unallocated space",NULL),TRUE,TRUE,0);
        GtkWidget *all=gtk_button_new_with_label("Use all space"); style(all,"link-button");
        g_signal_connect(all,"clicked",G_CALLBACK(editor_all),&editor); gtk_box_pack_end(GTK_BOX(legend),all,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(content),legend,FALSE,FALSE,0); editor_update(&editor);
        GtkWidget *instruction=label(mode==GROW?"Drag the right edge to grow. The start stays fixed.":"Drag either edge to resize. Drag the middle to move.","drag-instruction"); gtk_label_set_xalign(GTK_LABEL(instruction),.5); gtk_box_pack_start(GTK_BOX(content),instruction,FALSE,FALSE,0);
    }
    gboolean name_field=mode==CREATE || mode==ADJUST || mode==RENAME || mode==FORMAT;
    if (name_field && !strcmp(disk->label,"gpt"))
    {
        gtk_box_pack_start(GTK_BOX(content),label(mode==RENAME?"Partition name":"Name","field-label"),FALSE,FALSE,0);
        editor.name=gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(editor.name),part?(*part->name?part->name:part->fslabel):"Data"); gtk_entry_set_max_length(GTK_ENTRY(editor.name),36);
        gtk_box_pack_start(GTK_BOX(content),editor.name,FALSE,FALSE,0);
    }
    if (mode==CREATE || mode==ADJUST || mode==FORMAT)
    {
        GtkWidget *fs_label=label("Filesystem","field-label"); style(fs_label,"field-next"); gtk_box_pack_start(GTK_BOX(content),fs_label,FALSE,FALSE,0);
        const char *current="ext4";
        if (part) current=*part->format?part->format:part->fs;
        editor.fs=filesystem_combo(current,mode!=FORMAT);
        gtk_box_pack_start(GTK_BOX(content),editor.fs,FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(content),label(mode==FORMAT?"Formatting creates an empty filesystem.":"Choose a filesystem to make the new partition ready for your files.","form-hint"),FALSE,FALSE,0);
    }
    if (mode==GROW)
    {
        GtkWidget *result=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12), *text=gtk_box_new(GTK_ORIENTATION_VERTICAL,4); style(result,"growth-result");
        GtkWidget *check=image("check",18); gtk_widget_set_valign(check,GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(result),check,FALSE,FALSE,0);
        char *summary=*part->fs?g_strdup_printf("%s will grow with the partition.",fs_name(part)):g_strdup("The unformatted partition will grow."); gtk_box_pack_start(GTK_BOX(text),label(summary,NULL),FALSE,FALSE,0); g_free(summary);
        GtkWidget *note=label("Your files stay in place. Existing partitions are never moved.","growth-note"); gtk_label_set_line_wrap(GTK_LABEL(note),TRUE); gtk_box_pack_start(GTK_BOX(text),note,FALSE,FALSE,0);
        gtk_box_pack_start(GTK_BOX(result),text,TRUE,TRUE,0); gtk_box_pack_start(GTK_BOX(content),result,FALSE,FALSE,0);
    }
    gtk_widget_show_all(editor.dialog);
    if (gtk_dialog_run(GTK_DIALOG(editor.dialog))==GTK_RESPONSE_ACCEPT)
    {
        const char *fs=editor.fs?gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor.fs)):NULL;
        const char *type=fs?filesystem_type(disk,fs):NULL;
        if (fs && (!type || !*fs) && mode==FORMAT) message(app,"No filesystem selected","Install a supported filesystem formatter first.",NULL);
        else if (fs && !type) message(app,"Partition type unavailable","sfdisk did not report a suitable partition type for this filesystem.",NULL);
        else
        {
            remember(app);
            if (mode==CREATE)
            {
                part=partition_new(); part->number=1;
                while (find_part(disk->parts,part->number)) part->number++;
                g_free(part->uuid); part->uuid=!strcmp(disk->label,"gpt")?g_uuid_string_random():g_strdup("");
                g_ptr_array_add(disk->parts,part); app->selected=part->number;
            }
            if (mode<=GROW) { part->start=editor.start+editor.offset; part->size=editor.size; }
            if (editor.name) { g_free(part->name); part->name=g_strdup(gtk_entry_get_text(GTK_ENTRY(editor.name))); }
            if (fs)
            {
                g_free(part->format); part->format=g_strdup(fs);
                if (mode!=FORMAT) { g_free(part->type); part->type=g_strdup(type); }
            }
            if (mode==GROW) part->grow=*part->fs!=0;
            render(app,TRUE);
        }
    }
    gtk_widget_destroy(editor.dialog);
}

/* File exports are explicit and preserve the exact script used by the backend. */
static void save_layout(App *app)
{
    GtkWidget *dialog=gtk_file_chooser_dialog_new("Save partition layout",GTK_WINDOW(app->window),GTK_FILE_CHOOSER_ACTION_SAVE,"Cancel",GTK_RESPONSE_CANCEL,"Save",GTK_RESPONSE_ACCEPT,NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),"layout.sfdisk"); gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog),TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT)
    {
        char *path=gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)), *script=disk_script(app->disks->pdata[app->active]); GError *error=NULL;
        if (!g_file_set_contents(path,script,-1,&error)) { message(app,"Cannot save layout",error->message,NULL); g_clear_error(&error); }
        g_free(path); g_free(script);
    }
    gtk_widget_destroy(dialog);
}

/* All destructive intent is reviewed together before the asynchronous write job. */
static void acknowledge_busy(GtkToggleButton *checkbox, gpointer data)
{
    gtk_widget_set_sensitive(data,gtk_toggle_button_get_active(checkbox) && !g_object_get_data(G_OBJECT(data),"read-only"));
}

/* Present the exact plan and require an explicit acknowledgement for in-use disks. */
static void review(App *app)
{
    GtkWidget *dialog=make_dialog(app,"Review changes",680,"Apply changes"), *body=g_object_get_data(G_OBJECT(dialog),"body");
    GtkWidget *apply=gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);
    g_object_set_data(G_OBJECT(apply),"read-only",GINT_TO_POINTER(app->readonly)); gtk_widget_set_sensitive(apply,!app->readonly);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL), *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,16); gboolean busy=FALSE;
    gtk_container_add(GTK_CONTAINER(scroll),box); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll),TRUE); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),320);
    gtk_box_pack_start(GTK_BOX(body),scroll,TRUE,TRUE,0);
    for (guint i=0; i<app->disks->len; i++)
    {
        Disk *disk=app->disks->pdata[i]; if (!disk_changed(disk)) continue;
        busy|=disk->busy;
        char *title=g_strdup_printf("%s — %s",disk->title,disk->path), *plan=plan_description(disk);
        gtk_box_pack_start(GTK_BOX(box),label(title,"row-name"),FALSE,FALSE,0);
        GtkWidget *text=label(plan,NULL); gtk_label_set_line_wrap(GTK_LABEL(text),TRUE); gtk_label_set_selectable(GTK_LABEL(text),TRUE);
        gtk_box_pack_start(GTK_BOX(box),text,FALSE,FALSE,0); g_free(title); g_free(plan);
    }
    if (busy)
    {
        GtkWidget *warning=label("This disk is in use. Changes affecting mounted filesystems can destroy data or stop the running system. Nothing will be unmounted automatically. Some changes may require a restart.","in-use-warning");
        gtk_label_set_line_wrap(GTK_LABEL(warning),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(warning),70); gtk_box_pack_start(GTK_BOX(body),warning,FALSE,FALSE,16);
        GtkWidget *checkbox=gtk_check_button_new_with_label("I accept the risk of changing disks that are in use.");
        gtk_widget_set_sensitive(apply,FALSE);
        g_signal_connect(checkbox,"toggled",G_CALLBACK(acknowledge_busy),apply); gtk_box_pack_start(GTK_BOX(body),checkbox,FALSE,FALSE,0);
    }
    gtk_widget_show_all(dialog); int response=gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    if (response==GTK_RESPONSE_ACCEPT && !app->readonly) start_job(app,APPLY,busy);
}

/* Properties use aligned metadata rows rather than an unstructured message box. */
static void properties(App *app, Disk *disk)
{
    GtkWidget *dialog=make_dialog(app,"Disk properties",500,"Close"), *body=g_object_get_data(G_OBJECT(dialog),"body");
    GtkWidget *cancel=gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL); gtk_widget_hide(cancel); gtk_widget_set_no_show_all(cancel,TRUE);
    GtkWidget *grid=gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid),14); gtk_grid_set_column_spacing(GTK_GRID(grid),24);
    char *size=format_size(disk->bytes), *sector=g_strdup_printf("%u bytes",disk->sector), *table=g_ascii_strup(disk->label,-1);
    const char *names[]={"Device","Capacity","Partition table","Logical sector","Connection","Device type","Serial","WWN"};
    const char *values[]={disk->path,size,!strcmp(table,"DOS")?"MBR":*table?table:"None",sector,*disk->transport?disk->transport:"Not reported",disk->kind,*disk->serial?disk->serial:"Not reported",*disk->wwn?disk->wwn:"Not reported"};
    for (guint i=0;i<G_N_ELEMENTS(names);i++)
    {
        GtkWidget *key=label(names[i],"property-key"), *value=label(values[i],NULL); ellipsize(value); gtk_widget_set_hexpand(value,TRUE);
        gtk_grid_attach(GTK_GRID(grid),key,0,i,1,1); gtk_grid_attach(GTK_GRID(grid),value,1,i,1,1);
    }
    gtk_box_pack_start(GTK_BOX(body),grid,FALSE,FALSE,0); gtk_widget_show_all(dialog); gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    g_free(size); g_free(sector); g_free(table);
}

/* Discarding a plan never claims to undo operations already written to a disk. */
static gboolean discard_plan(App *app, const char *accept)
{
    GtkWidget *dialog=make_dialog(app,"Discard pending changes?",500,accept), *body=g_object_get_data(G_OBJECT(dialog),"body");
    gtk_button_set_label(GTK_BUTTON(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL)),"Keep editing");
    GtkWidget *text=label("The pending plan will be discarded. Your disks will remain unchanged.",NULL); gtk_label_set_line_wrap(GTK_LABEL(text),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(text),50); gtk_box_pack_start(GTK_BOX(body),text,FALSE,FALSE,0);
    gtk_widget_show_all(dialog); int response=gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog); return response==GTK_RESPONSE_ACCEPT;
}

/* Buttons and double-clicks share the same explicit, cancellable new-table dialog. */
static void new_table_dialog(App *app)
{
    Disk *disk=app->disks->pdata[app->active];
    GtkWidget *dialog=make_dialog(app,"New partition table",500,"Add to plan");
    GtkWidget *box=g_object_get_data(G_OBJECT(dialog),"body"), *combo=gtk_combo_box_text_new(); gtk_box_set_spacing(GTK_BOX(box),12);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"gpt","GPT — Modern systems"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),"dos","MBR — Legacy systems"); gtk_combo_box_set_active(GTK_COMBO_BOX(combo),0);
    gtk_box_pack_start(GTK_BOX(box),label("This replaces all existing partitions on the selected device.",NULL),FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(box),combo,FALSE,FALSE,0);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT)
    {
        remember(app); GError *error=NULL;
        if (!engine_new_table(disk,gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)),&error)) { message(app,"Cannot create table",error->message,NULL); g_clear_error(&error); }
        app->selected=-1; app->free_start=0; render(app,TRUE);
    }
    gtk_widget_destroy(dialog);
}

/* Reuse enabled toolbar actions so double-clicks respect the same editing limits. */
static void activate_selection(App *app)
{
    if (app->working || !app->disks->len) return;
    Disk *disk=app->disks->pdata[app->active];
    if (strcmp(disk->label,"gpt") && strcmp(disk->label,"dos")) { new_table_dialog(app); return; }
    GtkWidget *target=app->create;
    if (selected_part(app)) target=gtk_widget_is_sensitive(app->resize)?app->resize:app->edit;
    if (gtk_widget_is_sensitive(target)) gtk_button_clicked(GTK_BUTTON(target));
    else properties(app,disk);
}

/* Route the small toolbar directly to its focused native dialog or model edit. */
static void action(GtkButton *widget, gpointer data)
{
    App *app=data; const char *command=g_object_get_data(G_OBJECT(widget),"action");
    if (app->working) return;
    if (!strcmp(command,"refresh"))
    {
        if (app->history->len)
        {
            if (!discard_plan(app,"Discard and refresh")) return;
        }
        start_job(app,SCAN,FALSE); return;
    }
    if (!app->disks->len) return;
    Disk *disk=app->disks->pdata[app->active]; Partition *part=selected_part(app);
    if (!strcmp(command,"create")) edit_partition(app,CREATE);
    if (!strcmp(command,"edit")) edit_partition(app,RENAME);
    if (!strcmp(command,"resize")) edit_partition(app,part->origin?GROW:ADJUST);
    if (!strcmp(command,"format")) edit_partition(app,FORMAT);
    if (!strcmp(command,"delete"))
    {
        remember(app);
        for (guint i=0; i<disk->parts->len; i++) if (disk->parts->pdata[i]==part) { g_ptr_array_remove_index(disk->parts,i); break; }
        app->selected=-1; app->free_start=0; render(app,TRUE);
    }
    if (!strcmp(command,"repair")) { remember(app); disk->last=capacity_last(disk); disk->repair=TRUE; render(app,TRUE); }
    if (!strcmp(command,"undo") && app->history->len)
    {
        Undo *undo=app->history->pdata[app->history->len-1]; app->active=undo->disk;
        disk_free(app->disks->pdata[app->active]); app->disks->pdata[app->active]=undo->before; undo->before=NULL;
        g_ptr_array_remove_index(app->history,app->history->len-1); app->selected=-1; app->free_start=0; render(app,TRUE);
    }
    if (!strcmp(command,"new-table")) new_table_dialog(app);
    if (!strcmp(command,"review")) review(app);
    if (!strcmp(command,"save")) save_layout(app);
    if (!strcmp(command,"info")) properties(app,disk);
    if (!strcmp(command,"check")) start_job(app,CHECK,FALSE);
}

/* A worker owns immutable plans and its result log while GTK keeps responding. */
static void job_free(gpointer data)
{
    Job *job=data;
    g_ptr_array_unref(job->plans); g_free(job->backup); g_free(job->only); g_string_free(job->log,TRUE); g_free(job);
}

/* Discovery and destructive tools never run in the GTK event thread. */
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)cancel; Job *job=data; GError *error=NULL;
    if (!engine_init(&error)) { g_task_return_error(task,error); return; }
    if (job->type==CHECK)
    {
        char *output=NULL; gboolean result=engine_check(job->plans->pdata[0],&output,&error);
        if (output) g_string_append(job->log,output);
        g_free(output);
        if (!result) g_task_return_error(task,error); else g_task_return_pointer(task,NULL,NULL);
        return;
    }
    if (job->type==APPLY)
    {
        for (guint i=0; i<job->plans->len; i++)
            if (!engine_apply(job->plans->pdata[i],job->backup,job->log,&job->options,&error)) { g_task_return_error(task,error); return; }
        g_task_return_pointer(task,NULL,NULL); return;
    }
    GPtrArray *result=engine_scan(job->only,job->include_loop,&error);
    if (!result) g_task_return_error(task,error); else g_task_return_pointer(task,result,(GDestroyNotify)g_ptr_array_unref);
}

/* Always reload after writes, including partial failures; Undo is not a disk rollback. */
static void job_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; App *app=data; GTask *task=G_TASK(result); Job *job=g_task_get_task_data(task); GError *error=NULL;
    GPtrArray *disks=g_task_propagate_pointer(task,&error);
    app->working=FALSE; gtk_spinner_stop(GTK_SPINNER(app->spinner)); gtk_widget_hide(app->spinner); gtk_widget_set_sensitive(app->body,TRUE);
    if (job->type==CHECK)
    {
        render(app,TRUE); message(app,error?"Partition-table check failed":"Partition table checked",error?error->message:"sfdisk completed the partition-table check. Filesystems were not checked.",job->log->str); g_clear_error(&error); return;
    }
    if (job->type==APPLY)
    {
        const char *title=job->options.reboot_required?"Changes written — restart required":"Changes applied";
        const char *note=job->options.reboot_required?"The on-disk layout has changed. Linux still uses some previous partition entries. Restart before using the changed partitions.":"The requested disk and filesystem changes are complete.";
        message(app,error?"Disk operations stopped":title,error?error->message:note,job->log->str);
        g_ptr_array_set_size(app->disks,0); g_ptr_array_set_size(app->history,0);
        clear(app->sidebar); clear(app->rows); clear(app->details); gtk_widget_queue_draw(app->map);
        g_clear_error(&error); start_job(app,SCAN,FALSE); return;
    }
    if (error) { message(app,"Cannot read disks",error->message,NULL); gtk_label_set_text(GTK_LABEL(app->status),error->message); g_clear_error(&error); return; }
    g_ptr_array_unref(app->disks); app->disks=disks; g_ptr_array_set_size(app->history,0);
    app->active=0; app->selected=-1; app->free_start=0;
    gtk_stack_set_visible_child_name(GTK_STACK(app->content),disks->len?"disk":"empty");
    if (disks->len) { render_sidebar(app); render(app,TRUE); }
    else
    {
        render_sidebar(app);
        gtk_window_set_title(GTK_WINDOW(app->window),"Partition Manager");
        gtk_label_set_text(GTK_LABEL(app->header_device),""); clear(app->header_icon); clear(app->status_icon);
        gtk_label_set_text(GTK_LABEL(app->status),"No disks were found.");
        gtk_widget_set_sensitive(app->undo,FALSE); gtk_widget_set_sensitive(app->review,FALSE);
    }
}

/* Copy selected plans before disabling editing and starting the worker. */
static void start_job(App *app, JobType type, gboolean allow_busy)
{
    Job *job=g_new0(Job,1); job->plans=g_ptr_array_new_with_free_func(disk_free); job->log=g_string_new(NULL); job->only=g_strdup(app->only); job->type=type;
    job->options.allow_busy=allow_busy; job->include_loop=app->include_loop;
    job->backup=g_build_filename(g_get_user_state_dir(),"partition-manager","backups",NULL);
    if (type==APPLY)
        for (guint i=0; i<app->disks->len; i++) if (disk_changed(app->disks->pdata[i])) g_ptr_array_add(job->plans,disk_copy(app->disks->pdata[i]));
    if (type==CHECK) g_ptr_array_add(job->plans,disk_copy(app->disks->pdata[app->active]));
    app->working=TRUE; gtk_widget_set_sensitive(app->body,FALSE); gtk_widget_set_sensitive(app->undo,FALSE); gtk_widget_set_sensitive(app->review,FALSE);
    const char *status="Reading disks…";
    if (type==APPLY) status="Applying changes. Keep the disk connected…";
    if (type==CHECK) status="Checking partition table…";
    gtk_widget_hide(app->status_icon); gtk_widget_show(app->spinner); gtk_label_set_text(GTK_LABEL(app->status),status); gtk_spinner_start(GTK_SPINNER(app->spinner));
    GTask *task=g_task_new(NULL,NULL,job_done,app); g_task_set_task_data(task,job,job_free); g_task_run_in_thread(task,worker); g_object_unref(task);
}

/* Closing cannot terminate an in-progress filesystem operation or silently lose edits. */
static gboolean close_window(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event; App *app=data;
    if (app->working) { message(app,"Disk operation in progress","Wait for the current operation to finish before closing Partition Manager.",NULL); return TRUE; }
    gboolean pending=FALSE;
    for (guint i=0; i<app->disks->len; i++) pending|=disk_changed(app->disks->pdata[i]);
    if (pending)
    {
        if (!discard_plan(app,"Discard and close")) return TRUE;
    }
    gtk_main_quit(); return FALSE;
}

/* Construct the approved single-window Workspace with native GTK3 controls. */
int ui_run(int argc, char **argv, const char *only, gboolean readonly, gboolean include_loop)
{
    gtk_init(&argc,&argv); App app={.active=0,.selected=-1,.readonly=readonly,.include_loop=include_loop,.only=g_strdup(only)};
    app.disks=g_ptr_array_new_with_free_func(disk_free); app.history=g_ptr_array_new_with_free_func(undo_free);
    GtkCssProvider *css=gtk_css_provider_new(); gtk_css_provider_load_from_resource(css,"/org/slax/partition-manager/style.css");
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION); g_object_unref(css);
    app.window=gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_wmclass(GTK_WINDOW(app.window),"partition-manager","PartitionManager");
    gtk_window_set_title(GTK_WINDOW(app.window),"Partition Manager"); gtk_window_set_default_size(GTK_WINDOW(app.window),999,666); gtk_window_set_position(GTK_WINDOW(app.window),GTK_WIN_POS_CENTER);
    GdkPixbuf *mark=gdk_pixbuf_new_from_resource_at_scale("/org/slax/partition-manager/app-icon.png",128,128,TRUE,NULL);
    gtk_window_set_default_icon(mark); g_clear_object(&mark); style(app.window,"partitions");
    app.size_group=gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    GtkWidget *header=gtk_header_bar_new(); gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header),TRUE); gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header),":minimize,maximize,close");
    GtkWidget *brand=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    GdkPixbuf *brand_icon=gdk_pixbuf_new_from_resource_at_scale("/org/slax/partition-manager/app-icon.png",30,30,TRUE,NULL);
    gtk_box_pack_start(GTK_BOX(brand),gtk_image_new_from_pixbuf(brand_icon),FALSE,FALSE,0); g_object_unref(brand_icon);
    gtk_box_pack_start(GTK_BOX(brand),label("Partition Manager","app-title"),FALSE,FALSE,0); gtk_widget_set_size_request(brand,160,-1); gtk_header_bar_pack_start(GTK_HEADER_BAR(header),brand);
    GtkWidget *header_device=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); style(header_device,"title-center");
    app.header_icon=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); app.header_device=label("",NULL); ellipsize(app.header_device); gtk_widget_set_size_request(app.header_device,240,-1);
    gtk_box_pack_start(GTK_BOX(header_device),app.header_icon,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(header_device),app.header_device,FALSE,FALSE,0);
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header),header_device); gtk_window_set_titlebar(GTK_WINDOW(app.window),header);
    GtkWidget *outer=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); gtk_container_add(GTK_CONTAINER(app.window),outer);
    app.body=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_box_pack_start(GTK_BOX(outer),app.body,TRUE,TRUE,0);
    GtkWidget *sidebar=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); style(sidebar,"sidebar"); style(sidebar,"sidebar-inner"); gtk_widget_set_size_request(sidebar,206,-1);
    GtkWidget *side_heading=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8); style(side_heading,"sidebar-label");
    gtk_box_pack_start(GTK_BOX(side_heading),label("DEVICES",NULL),TRUE,TRUE,0);
    GtkWidget *refresh=button(&app,NULL,"hard-drive","refresh"); style(refresh,"link-button"); gtk_widget_set_tooltip_text(refresh,"Refresh devices"); gtk_box_pack_end(GTK_BOX(side_heading),refresh,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(sidebar),side_heading,FALSE,FALSE,0);
    GtkWidget *side_scroll=gtk_scrolled_window_new(NULL,NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    app.sidebar=gtk_list_box_new(); style(app.sidebar,"device-list"); gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.sidebar),GTK_SELECTION_SINGLE); gtk_container_add(GTK_CONTAINER(side_scroll),app.sidebar); gtk_box_pack_start(GTK_BOX(sidebar),side_scroll,TRUE,TRUE,0);
    GtkWidget *footnote=label("Select a disk, then a partition or an unallocated area.","sidebar-footnote"); gtk_label_set_line_wrap(GTK_LABEL(footnote),TRUE); gtk_label_set_max_width_chars(GTK_LABEL(footnote),22); gtk_box_pack_start(GTK_BOX(sidebar),footnote,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(app.body),sidebar,FALSE,FALSE,0);
    app.content=gtk_stack_new(); gtk_box_pack_start(GTK_BOX(app.body),app.content,TRUE,TRUE,0);
    GtkWidget *empty=gtk_label_new("No disks were found."); style(empty,"muted"); gtk_stack_add_named(GTK_STACK(app.content),empty,"empty");
    GtkWidget *main_clip=gtk_scrolled_window_new(NULL,NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(main_clip),GTK_POLICY_NEVER,GTK_POLICY_EXTERNAL); gtk_stack_add_named(GTK_STACK(app.content),main_clip,"disk");
    GtkWidget *main=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); style(main,"disk-main"); gtk_container_add(GTK_CONTAINER(main_clip),main);
    GtkWidget *heading=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,20), *titles=gtk_box_new(GTK_ORIENTATION_VERTICAL,6); style(heading,"disk-heading");
    app.heading=label("Partition Manager","disk-title"); app.subtitle=label("Select a device to begin.","muted");
    ellipsize(app.heading);
    gtk_box_pack_start(GTK_BOX(titles),app.heading,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(titles),app.subtitle,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(heading),titles,TRUE,TRUE,0);
    GtkWidget *tools=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,7); gtk_widget_set_valign(tools,GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(tools),button(&app,"Check","check","check"),FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(tools),button(&app,NULL,"circle-info","info"),FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(tools),button(&app,NULL,"floppy-disk","save"),FALSE,FALSE,0); gtk_box_pack_end(GTK_BOX(heading),tools,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(main),heading,FALSE,FALSE,0); app.notice=gtk_box_new(GTK_ORIENTATION_VERTICAL,8); gtk_box_pack_start(GTK_BOX(main),app.notice,FALSE,FALSE,0);
    GtkWidget *toolbar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6); style(toolbar,"toolbar");
    app.create=button(&app,"New","plus","create"); app.edit=button(&app,"Rename","pen-to-square","edit"); app.resize=button(&app,"Resize","up-right-and-down-left-from-center","resize"); app.format=button(&app,"Format","eraser","format"); app.remove=button(&app,NULL,"trash-can","delete");
    gtk_box_pack_start(GTK_BOX(toolbar),app.create,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(toolbar),app.edit,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(toolbar),app.resize,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(toolbar),app.format,FALSE,FALSE,0);
    GtkWidget *divider=gtk_separator_new(GTK_ORIENTATION_VERTICAL); style(divider,"toolbar-divider"); gtk_box_pack_start(GTK_BOX(toolbar),divider,FALSE,FALSE,5);
    gtk_box_pack_start(GTK_BOX(toolbar),app.remove,FALSE,FALSE,0); GtkWidget *new_table=button(&app,"New table…",NULL,"new-table"); style(new_table,"link-button"); gtk_box_pack_end(GTK_BOX(toolbar),new_table,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(main),toolbar,FALSE,FALSE,0);
    app.map=gtk_drawing_area_new(); style(app.map,"disk-map"); gtk_widget_set_size_request(app.map,-1,68); gtk_widget_add_events(app.map,GDK_BUTTON_PRESS_MASK); gtk_box_pack_start(GTK_BOX(main),app.map,FALSE,FALSE,0);
    GtkWidget *map_caption=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,15); style(map_caption,"map-caption"); app.allocated=label("",NULL); app.available=label("","free-count"); gtk_box_pack_start(GTK_BOX(map_caption),app.allocated,TRUE,TRUE,0); gtk_box_pack_end(GTK_BOX(map_caption),app.available,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(main),map_caption,FALSE,FALSE,0);
    GtkWidget *editing=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,22), *column=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    GtkWidget *table_header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12); style(table_header,"table-header");
    GtkWidget *name_header=label("Partition",NULL), *fs_header=label("Filesystem",NULL), *size_header=label("Size",NULL); gtk_label_set_xalign(GTK_LABEL(size_header),1); gtk_widget_set_size_request(size_header,85,-1);
    ellipsize(fs_header); gtk_size_group_add_widget(app.size_group,size_header);
    gtk_box_pack_start(GTK_BOX(table_header),table_columns(name_header,fs_header),TRUE,TRUE,0); gtk_box_pack_start(GTK_BOX(table_header),size_header,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(column),table_header,FALSE,FALSE,0);
    app.row_scroll=gtk_scrolled_window_new(NULL,NULL); gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(app.row_scroll),TRUE); gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(app.row_scroll),342);
    app.rows=gtk_list_box_new(); style(app.rows,"partition-list"); gtk_list_box_set_selection_mode(GTK_LIST_BOX(app.rows),GTK_SELECTION_SINGLE); gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app.rows),FALSE); gtk_container_add(GTK_CONTAINER(app.row_scroll),app.rows);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app.row_scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC); gtk_box_pack_start(GTK_BOX(column),app.row_scroll,TRUE,TRUE,0); gtk_box_pack_start(GTK_BOX(editing),column,TRUE,TRUE,0);
    GtkWidget *details_clip=gtk_scrolled_window_new(NULL,NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(details_clip),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC); gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(details_clip),TRUE);
    app.details=gtk_box_new(GTK_ORIENTATION_VERTICAL,0); style(app.details,"details"); gtk_widget_set_size_request(app.details,238,-1); gtk_container_add(GTK_CONTAINER(details_clip),app.details); gtk_box_pack_start(GTK_BOX(editing),details_clip,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(main),editing,TRUE,TRUE,0);
    GtkWidget *footer=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,9); style(footer,"footer");
    app.status_icon=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_box_pack_start(GTK_BOX(footer),app.status_icon,FALSE,FALSE,0);
    app.spinner=gtk_spinner_new(); gtk_widget_set_size_request(app.spinner,17,17); gtk_box_pack_start(GTK_BOX(footer),app.spinner,FALSE,FALSE,0); app.status=label("Reading disks…","pending-status"); gtk_label_set_ellipsize(GTK_LABEL(app.status),PANGO_ELLIPSIZE_END); gtk_box_pack_start(GTK_BOX(footer),app.status,TRUE,TRUE,0);
    app.undo=button(&app,"Undo","arrow-rotate-left","undo"); app.review=button(&app,"Review changes",NULL,"review"); style(app.review,"suggested-action");
    gtk_box_pack_start(GTK_BOX(footer),app.undo,FALSE,FALSE,0); gtk_box_pack_start(GTK_BOX(footer),app.review,FALSE,FALSE,0); gtk_box_pack_end(GTK_BOX(outer),footer,FALSE,FALSE,0);
    g_signal_connect(app.sidebar,"row-selected",G_CALLBACK(disk_selected),&app); g_signal_connect(app.rows,"row-selected",G_CALLBACK(row_selected),&app);
    g_signal_connect(app.rows,"row-activated",G_CALLBACK(row_activated),&app);
    g_signal_connect(app.map,"draw",G_CALLBACK(map_draw),&app); g_signal_connect(app.map,"button-press-event",G_CALLBACK(map_press),&app); g_signal_connect(app.window,"delete-event",G_CALLBACK(close_window),&app);
    gtk_widget_show_all(app.window); start_job(&app,SCAN,FALSE); gtk_main();
    g_ptr_array_unref(app.disks); g_ptr_array_unref(app.history); g_object_unref(app.size_group); g_free(app.only); return 0;
}
