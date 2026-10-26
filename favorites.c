/*
 * favorites.c - Favorite files plugin for Geany (>= 1.38, tested against 2.0)
 *
 * - "Favorites" tab in the sidebar (click/Enter opens, drag to reorder,
 *   Delete removes, right-click for a context menu)
 * - Tools > Favorites submenu for quick access
 * - Editor right-click menu entry to add/remove the current file
 * - Keybindings: add current file, remove current file, focus the sidebar tab
 * - Stored in ~/.config/geany/plugins/favorites/favorites.conf
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <geanyplugin.h>
#include <glib/gstdio.h>

#define GD (fav_plugin->geany_data)

enum { COL_NAME, COL_PATH, COL_DIR, COL_MISSING, N_COLS };
enum { KB_ADD_CURRENT, KB_REMOVE_CURRENT, KB_FOCUS, KB_COUNT };

static GeanyPlugin  *fav_plugin;
static GtkListStore *store;
static GtkWidget    *page;       /* sidebar page */
static GtkWidget    *tree;
static GtkWidget    *menu_root;  /* Tools > Favorites */
static GtkWidget    *menu_sub;
static GtkWidget    *editor_item; /* editor right-click menu */
static gchar        *config_file;
static gboolean      loading;    /* suppress saving while (re)loading */
static guint         rebuild_id; /* pending idle menu rebuild */

static void favorites_changed(void);

/* ------------------------------------------------------------------ helpers */

/* Paths are kept in UTF-8 (like GeanyDocument->file_name and GKeyFile);
 * converted to locale encoding only when touching the filesystem. */
static gboolean file_missing(const gchar *utf8)
{
	gchar *locale = utils_get_locale_from_utf8(utf8);
	gboolean missing = !g_file_test(locale, G_FILE_TEST_EXISTS);
	g_free(locale);
	return missing;
}

static gboolean find_path(const gchar *utf8, GtkTreeIter *out)
{
	GtkTreeModel *model = GTK_TREE_MODEL(store);
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

	while (valid)
	{
		gchar *p;
		gboolean match;

		gtk_tree_model_get(model, &iter, COL_PATH, &p, -1);
		match = g_strcmp0(p, utf8) == 0;
		g_free(p);
		if (match)
		{
			if (out)
				*out = iter;
			return TRUE;
		}
		valid = gtk_tree_model_iter_next(model, &iter);
	}
	return FALSE;
}

static gboolean doc_is_saved(GeanyDocument *doc)
{
	return DOC_VALID(doc) && doc->file_name && doc->real_path;
}

static gboolean find_doc(GeanyDocument *doc, GtkTreeIter *out)
{
	return DOC_VALID(doc) && doc->file_name && find_path(doc->file_name, out);
}

static void append_row(const gchar *utf8)
{
	gchar *name = g_path_get_basename(utf8);
	gchar *dir = g_path_get_dirname(utf8);
	const gchar *home = g_get_home_dir();
	gchar *shown_dir;

	if (home && g_str_has_prefix(dir, home) &&
		(dir[strlen(home)] == '\0' || dir[strlen(home)] == G_DIR_SEPARATOR))
		shown_dir = g_strconcat("~", dir + strlen(home), NULL);
	else
		shown_dir = g_strdup(dir);

	gtk_list_store_insert_with_values(store, NULL, -1,
		COL_NAME, name, COL_PATH, utf8, COL_DIR, shown_dir,
		COL_MISSING, file_missing(utf8), -1);

	g_free(shown_dir);
	g_free(dir);
	g_free(name);
}

/* Returns TRUE if added, FALSE if already present. */
static gboolean add_path(const gchar *utf8)
{
	if (find_path(utf8, NULL))
		return FALSE;
	append_row(utf8);
	return TRUE;
}

static void open_path(const gchar *utf8)
{
	gchar *locale;

	if (file_missing(utf8))
	{
		ui_set_statusbar(TRUE, _("Favorite not found: %s"), utf8);
		return;
	}
	locale = utils_get_locale_from_utf8(utf8);
	document_open_file(locale, FALSE, NULL, NULL);  /* switches tab if already open */
	g_free(locale);
}

static gchar *get_selected_path(GtkTreeIter *iter_out)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *path = NULL;

	if (gtk_tree_selection_get_selected(sel, &model, &iter))
	{
		gtk_tree_model_get(model, &iter, COL_PATH, &path, -1);
		if (iter_out)
			*iter_out = iter;
	}
	return path;
}

/* ------------------------------------------------------------- persistence */

static void save_favorites(void)
{
	GKeyFile *kf = g_key_file_new();
	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	GtkTreeModel *model = GTK_TREE_MODEL(store);
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
	gchar *dir;
	GError *err = NULL;

	while (valid)
	{
		gchar *p;
		gtk_tree_model_get(model, &iter, COL_PATH, &p, -1);
		g_ptr_array_add(paths, p);
		valid = gtk_tree_model_iter_next(model, &iter);
	}
	g_ptr_array_add(paths, NULL);  /* keeps pdata non-NULL when empty */

	g_key_file_set_string_list(kf, "favorites", "files",
		(const gchar * const *) paths->pdata, paths->len - 1);

	dir = g_path_get_dirname(config_file);
	g_mkdir_with_parents(dir, 0755);
	if (!g_key_file_save_to_file(kf, config_file, &err))
	{
		g_warning("Favorites: cannot save %s: %s", config_file, err->message);
		g_error_free(err);
	}

	g_free(dir);
	g_ptr_array_free(paths, TRUE);
	g_key_file_free(kf);
}

static void load_favorites(void)
{
	GKeyFile *kf = g_key_file_new();

	if (g_key_file_load_from_file(kf, config_file, G_KEY_FILE_NONE, NULL))
	{
		gchar **files = g_key_file_get_string_list(kf, "favorites", "files", NULL, NULL);
		gchar **f;

		loading = TRUE;
		for (f = files; f && *f; f++)
			if (**f)
				add_path(*f);
		loading = FALSE;
		g_strfreev(files);
	}
	g_key_file_free(kf);
}

/* ----------------------------------------------------------------- actions */

static void add_current(void)
{
	GeanyDocument *doc = document_get_current();

	if (!doc_is_saved(doc))
	{
		ui_set_statusbar(FALSE, _("Favorites: save the document first."));
		return;
	}
	if (add_path(doc->file_name))
	{
		favorites_changed();
		ui_set_statusbar(FALSE, _("Added to favorites: %s"), doc->file_name);
	}
	else
		ui_set_statusbar(FALSE, _("Already in favorites: %s"), doc->file_name);
}

static void remove_current(void)
{
	GeanyDocument *doc = document_get_current();
	GtkTreeIter iter;

	if (find_doc(doc, &iter))
	{
		gtk_list_store_remove(store, &iter);  /* "row-deleted" saves */
		ui_set_statusbar(FALSE, _("Removed from favorites: %s"), doc->file_name);
	}
	else
		ui_set_statusbar(FALSE, _("Current file is not a favorite."));
}

static void remove_selected(void)
{
	GtkTreeIter iter;
	gchar *path = get_selected_path(&iter);

	if (path)
	{
		gtk_list_store_remove(store, &iter);
		g_free(path);
	}
}

static void add_files_dialog(void)
{
	GtkWidget *dialog = gtk_file_chooser_dialog_new(_("Add Favorites"),
		GTK_WINDOW(GD->main_widgets->window), GTK_FILE_CHOOSER_ACTION_OPEN,
		_("_Cancel"), GTK_RESPONSE_CANCEL, _("_Add"), GTK_RESPONSE_ACCEPT, NULL);
	GeanyDocument *doc = document_get_current();

	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
	if (DOC_VALID(doc) && doc->real_path)
	{
		gchar *dir = g_path_get_dirname(doc->real_path);
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dir);
		g_free(dir);
	}

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
		GSList *node;
		gboolean changed = FALSE;

		for (node = files; node; node = node->next)
		{
			gchar *utf8 = utils_get_utf8_from_locale(node->data);
			changed |= add_path(utf8);
			g_free(utf8);
		}
		g_slist_free_full(files, g_free);
		if (changed)
			favorites_changed();
	}
	gtk_widget_destroy(dialog);
}

static void focus_favorites(void)
{
	GtkNotebook *nb = GTK_NOTEBOOK(GD->main_widgets->sidebar_notebook);
	gint n;

	/* Geany hides the whole sidebar notebook; toggle via its own command
	 * so its internal state (and the View menu check item) stays in sync. */
	if (!gtk_widget_get_visible(GTK_WIDGET(nb)))
		keybindings_send_command(GEANY_KEY_GROUP_VIEW, GEANY_KEYS_VIEW_SIDEBAR);

	n = gtk_notebook_page_num(nb, page);
	if (n >= 0)
		gtk_notebook_set_current_page(nb, n);
	gtk_widget_grab_focus(tree);
}

/* -------------------------------------------------------------- Tools menu */

static void on_menu_open(GtkMenuItem *item, gpointer data)
{
	open_path(g_object_get_data(G_OBJECT(item), "fav-path"));
}

static GtkWidget *menu_append(GtkWidget *menu, const gchar *label, GCallback cb)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	if (cb)
		g_signal_connect(item, "activate", cb, NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

static void destroy_child(GtkWidget *child, gpointer data)
{
	gtk_widget_destroy(child);
}

static void rebuild_menu(void)
{
	GtkTreeModel *model = GTK_TREE_MODEL(store);
	GtkTreeIter iter;
	gboolean valid;

	gtk_container_foreach(GTK_CONTAINER(menu_sub), destroy_child, NULL);

	menu_append(menu_sub, _("_Add Current File"), G_CALLBACK(add_current));
	menu_append(menu_sub, _("_Remove Current File"), G_CALLBACK(remove_current));
	menu_append(menu_sub, _("Add _Files..."), G_CALLBACK(add_files_dialog));
	menu_append(menu_sub, _("Show _Sidebar Tab"), G_CALLBACK(focus_favorites));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu_sub), gtk_separator_menu_item_new());

	valid = gtk_tree_model_get_iter_first(model, &iter);
	if (!valid)
		gtk_widget_set_sensitive(menu_append(menu_sub, _("(no favorites)"), NULL), FALSE);

	while (valid)
	{
		gchar *name, *path;
		gboolean missing;
		GtkWidget *item;

		gtk_tree_model_get(model, &iter, COL_NAME, &name, COL_PATH, &path,
			COL_MISSING, &missing, -1);
		item = gtk_menu_item_new_with_label(name);  /* no mnemonic: '_' safe */
		gtk_widget_set_tooltip_text(item, path);
		gtk_widget_set_sensitive(item, !missing);
		g_object_set_data_full(G_OBJECT(item), "fav-path", path, g_free);
		g_signal_connect(item, "activate", G_CALLBACK(on_menu_open), NULL);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu_sub), item);
		g_free(name);
		valid = gtk_tree_model_iter_next(model, &iter);
	}
	gtk_widget_show_all(menu_sub);
}

static gboolean rebuild_menu_idle(gpointer data)
{
	rebuild_id = 0;
	rebuild_menu();
	return G_SOURCE_REMOVE;
}

/* The menu is rebuilt from an idle callback: changes are often triggered
 * by an item of that very menu, which must not be destroyed mid-activation. */
static void favorites_changed(void)
{
	if (loading)
		return;
	save_favorites();
	if (rebuild_id == 0)
		rebuild_id = g_idle_add(rebuild_menu_idle, NULL);
}

/* Fires on removal and at the end of a drag-and-drop reorder. */
static void on_row_deleted(GtkTreeModel *model, GtkTreePath *path, gpointer data)
{
	favorites_changed();
}

/* ---------------------------------------------------------- sidebar widget */

static void render_name(GtkTreeViewColumn *col, GtkCellRenderer *cell,
		GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	gchar *name, *dir, *markup;
	gboolean missing;

	gtk_tree_model_get(model, iter, COL_NAME, &name, COL_DIR, &dir, COL_MISSING, &missing, -1);
	markup = g_markup_printf_escaped("%s\n<small><span alpha=\"60%%\">%s</span></small>", name, dir);
	g_object_set(cell, "markup", markup, "strikethrough", missing, NULL);
	g_free(markup);
	g_free(dir);
	g_free(name);
}

static void on_row_activated(GtkTreeView *view, GtkTreePath *tpath,
		GtkTreeViewColumn *col, gpointer data)
{
	GtkTreeIter iter;

	if (gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, tpath))
	{
		gchar *path;
		gboolean missing;

		gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, COL_PATH, &path, -1);
		/* refresh the "missing" flag on use */
		missing = file_missing(path);
		gtk_list_store_set(store, &iter, COL_MISSING, missing, -1);
		open_path(path);
		g_free(path);
	}
}

static void on_ctx_open(void)
{
	gchar *path = get_selected_path(NULL);
	if (path)
	{
		open_path(path);
		g_free(path);
	}
}

static void on_ctx_copy_path(void)
{
	gchar *path = get_selected_path(NULL);
	if (path)
	{
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), path, -1);
		g_free(path);
	}
}

static void on_ctx_open_folder(void)
{
	gchar *path = get_selected_path(NULL);
	if (path)
	{
		gchar *locale = utils_get_locale_from_utf8(path);
		gchar *dir = g_path_get_dirname(locale);
		gchar *uri = g_filename_to_uri(dir, NULL, NULL);

		if (uri)
			gtk_show_uri_on_window(GTK_WINDOW(GD->main_widgets->window), uri,
				GDK_CURRENT_TIME, NULL);
		g_free(uri);
		g_free(dir);
		g_free(locale);
		g_free(path);
	}
}

static gboolean on_tree_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	GtkTreePath *tpath;
	GtkWidget *menu;

	if (event->type != GDK_BUTTON_PRESS || event->button != 3)
		return FALSE;

	if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (gint) event->x,
			(gint) event->y, &tpath, NULL, NULL, NULL))
		return FALSE;
	gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), tpath);
	gtk_tree_path_free(tpath);

	menu = gtk_menu_new();
	menu_append(menu, _("_Open"), G_CALLBACK(on_ctx_open));
	menu_append(menu, _("Open Containing _Folder"), G_CALLBACK(on_ctx_open_folder));
	menu_append(menu, _("_Copy Path"), G_CALLBACK(on_ctx_copy_path));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
	menu_append(menu, _("_Remove from Favorites"), G_CALLBACK(remove_selected));
	gtk_widget_show_all(menu);
	g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *) event);
	return TRUE;
}

static gboolean on_tree_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	if (event->keyval == GDK_KEY_Delete || event->keyval == GDK_KEY_KP_Delete)
	{
		remove_selected();
		return TRUE;
	}
	return FALSE;
}

static GtkToolItem *tool_button(const gchar *icon, const gchar *tip, GCallback cb)
{
	GtkToolItem *item = gtk_tool_button_new(
		gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR), NULL);
	gtk_widget_set_tooltip_text(GTK_WIDGET(item), tip);
	g_signal_connect(item, "clicked", cb, NULL);
	return item;
}

static GtkWidget *build_sidebar_page(void)
{
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *toolbar = gtk_toolbar_new();
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	GtkCellRenderer *cell = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *col = gtk_tree_view_column_new();

	gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar), GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
		tool_button("list-add", _("Add current file"), G_CALLBACK(add_current)), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
		tool_button("document-open", _("Add files..."), G_CALLBACK(add_files_dialog)), -1);
	gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
		tool_button("list-remove", _("Remove selected"), G_CALLBACK(remove_selected)), -1);

	tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW(tree), TRUE);
	gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(tree), TRUE);
	gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(tree), COL_PATH);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), TRUE);
	gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree), COL_NAME);
	g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
	gtk_tree_view_column_pack_start(col, cell, TRUE);
	gtk_tree_view_column_set_cell_data_func(col, cell, render_name, NULL, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

	g_signal_connect(tree, "row-activated", G_CALLBACK(on_row_activated), NULL);
	g_signal_connect(tree, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);
	g_signal_connect(tree, "key-press-event", G_CALLBACK(on_tree_key_press), NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), tree);

	gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
	gtk_widget_show_all(vbox);
	return vbox;
}

/* ------------------------------------------------------ editor popup menu */

static void toggle_current(void)
{
	if (find_doc(document_get_current(), NULL))
		remove_current();
	else
		add_current();
}

static void on_update_editor_menu(GObject *obj, const gchar *word, gint pos,
	GeanyDocument *doc, gpointer data)
{
	gboolean favorite = find_doc(doc, NULL);

	gtk_menu_item_set_label(GTK_MENU_ITEM(editor_item), favorite ?
		_("Remove from _Favorites") : _("Add to _Favorites"));
	gtk_widget_set_sensitive(editor_item, favorite || doc_is_saved(doc));
}

/* ------------------------------------------------------------- keybindings */

static void on_keybinding(guint key_id)
{
	switch (key_id)
	{
		case KB_ADD_CURRENT:    add_current(); break;
		case KB_REMOVE_CURRENT: remove_current(); break;
		case KB_FOCUS:          focus_favorites(); break;
	}
}

/* ------------------------------------------------------------ plugin hooks */

static gboolean fav_init(GeanyPlugin *plugin, gpointer pdata)
{
	GeanyKeyGroup *group;

	fav_plugin = plugin;
	config_file = g_build_filename(GD->app->configdir, "plugins", "favorites",
		"favorites.conf", NULL);

	store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_STRING, G_TYPE_BOOLEAN);
	load_favorites();
	g_signal_connect(store, "row-deleted", G_CALLBACK(on_row_deleted), NULL);

	page = build_sidebar_page();
	gtk_notebook_append_page(GTK_NOTEBOOK(GD->main_widgets->sidebar_notebook),
		page, gtk_label_new(_("Favorites")));

	menu_root = gtk_menu_item_new_with_mnemonic(_("_Favorites"));
	menu_sub = gtk_menu_new();
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_root), menu_sub);
	gtk_container_add(GTK_CONTAINER(GD->main_widgets->tools_menu), menu_root);
	gtk_widget_show(menu_root);
	rebuild_menu();

	editor_item = menu_append(GD->main_widgets->editor_menu, _("Add to _Favorites"),
		G_CALLBACK(toggle_current));
	gtk_widget_show(editor_item);
	plugin_signal_connect(plugin, NULL, "update-editor-menu", FALSE,
		G_CALLBACK(on_update_editor_menu), NULL);

	group = plugin_set_key_group(plugin, "favorites", KB_COUNT, NULL);
	keybindings_set_item(group, KB_ADD_CURRENT, on_keybinding, 0, 0,
		"add_current", _("Add current file to favorites"), NULL);
	keybindings_set_item(group, KB_REMOVE_CURRENT, on_keybinding, 0, 0,
		"remove_current", _("Remove current file from favorites"), NULL);
	keybindings_set_item(group, KB_FOCUS, on_keybinding, 0, 0,
		"focus", _("Show favorites sidebar tab"), NULL);

	return TRUE;
}

static void fav_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	if (rebuild_id)
		g_source_remove(rebuild_id);
	rebuild_id = 0;
	gtk_widget_destroy(page);       /* also removes it from the notebook */
	gtk_widget_destroy(menu_root);
	gtk_widget_destroy(editor_item);
	g_object_unref(store);
	g_free(config_file);
	page = tree = menu_root = menu_sub = editor_item = NULL;
	store = NULL;
	config_file = NULL;
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
#ifdef LOCALEDIR
	main_locale_init(LOCALEDIR, GETTEXT_PACKAGE);
#endif
	plugin->info->name = _("Favorites");
	plugin->info->description =
		_("Keep a list of favorite files in the sidebar and Tools menu");
	plugin->info->version = "0.1";
	plugin->info->author = "Stephane Cerveau <scerveau@gmail.com>";

	plugin->funcs->init = fav_init;
	plugin->funcs->cleanup = fav_cleanup;

	GEANY_PLUGIN_REGISTER(plugin, 225);
}
