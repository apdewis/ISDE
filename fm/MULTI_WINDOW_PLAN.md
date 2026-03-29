# FM Single-Instance Multi-Window Refactor Plan

## Static/Global State Inventory

Complete census of every file-scoped static that must move:

### fm.c
- `rename_shell`, `rename_fm`, `rename_index` — rename dialog
- `delete_shell`, `delete_fm` — delete confirmation dialog
- `empty_trash_shell` — empty trash dialog
- `ctx_shell`, `ctx_list`, `ctx_fm`, `ctx_in_trash` — context menu popup
- `dyn_labels`, `dyn_actions`, `dyn_nitems` — dynamic context menu arrays
- `ow_indices[]`, `ow_count`, `ow_label_buf[]`, `ow_file_path` — "Open with" state
- `shortcut_fm` — Fm pointer for all Xt action procedures
- `base_labels[]`, `base_actions[]`, `ctx_trash_labels[]`, `ctx_trash_actions[]` — **constant, keep static**
- `fm_actions[]`, `fm_translations[]` — **constant, keep static, register once**

### fileview.c
- `last_click_index`, `last_click_time` — double-click detection
- `labels`, `icons`, `trunc_names`, `trunc_count` — IconView backing arrays

### clipboard.c
- `g_fm` — global Fm pointer for selection convert callback

### places.c
- `places`, `nplaces`, `places_cap` — place entry array
- `sections`, `nsections` — section widget tracking

### dnd.c
- `drag_fm` — global Fm pointer for drag actions
- `saved_press`, `press_valid` — drag initiation state
- `drag_paths`, `ndrag_paths` — paths being dragged
- `drop_was_noop` — drop result flag
- `dnd_actions[]` — **constant, keep static, register once**

### icons.c
- `icon_folder_data`, `icon_file_data`, `icon_exec_data`, `icon_image_data` — cached SVG strings
- `configured_icon_theme` — theme name

### main.c
- `static Fm fm` — the single instance

---

## Phase 1: Eliminate Global/Static State

Each step produces a compilable, testable binary.

### Step 1.1: Split Fm into FmApp + FmWindow

**Files:** `fm.h`

Create two structs:

```c
typedef struct FmApp {
    XtAppContext    app;
    Widget          first_toplevel;  /* from XtAppInitialize */

    /* Shared caches */
    IsdeDesktopEntry **desktop_entries;
    int                ndesktop;
    IsdeDBus          *dbus;

    /* Icon cache (moved from icons.c statics) */
    char *icon_folder, *icon_file, *icon_exec, *icon_image, *icon_theme;

    /* Clipboard atoms (shared — atom values are per-display) */
    xcb_atom_t  atom_clipboard, atom_targets, atom_uri_list;
    xcb_atom_t  atom_gnome_files, atom_utf8_string;

    /* Window tracking */
    FmWindow  **windows;
    int         nwindows;
    FmWindow   *clipboard_owner;  /* which window owns CLIPBOARD */

    int         running;
} FmApp;
```

Rename `Fm` → `FmWindow`, add `FmApp *app_state` back-pointer, and absorb all per-window statics (listed above) as fields. Existing fields (widgets, cwd, entries, history, etc.) stay.

**Test:** Header compiles. Nothing else compiles yet — commit header, proceed.

### Step 1.2: Move icon cache into FmApp

**Files:** `icons.c`, `fm.h`

- Remove 5 file statics.
- `icons_init(FmApp *)`, `icons_for_entry(FmApp *, const FmEntry *)`, `icons_cleanup(FmApp *)`.
- Update call in `browser.c` to pass `win->app_state`.

**Test:** Build succeeds, icons display correctly.

### Step 1.3: Move places sidebar statics into FmWindow

**Files:** `places.c`, `fm.h`

- Move `places`, `nplaces`, `places_cap`, `sections`, `nsections` into FmWindow (or a `FmPlaces` sub-struct).
- All functions take `FmWindow *win`.

**Test:** Build succeeds, sidebar works.

### Step 1.4: Move fileview statics into FmWindow

**Files:** `fileview.c`, `fm.h`

- `last_click_index`, `last_click_time` → `win->last_click_index`, `win->last_click_time`.
- Static `labels`, `icons`, `trunc_names`, `trunc_count` → `win->fv_labels`, `win->fv_icons`, `win->fv_trunc_names`, `win->fv_trunc_count`.

**Test:** Build succeeds, double-click opens items, icons display.

### Step 1.5: Move clipboard statics into FmWindow

**Files:** `clipboard.c`, `fm.h`

- Remove `g_fm`.
- `convert_selection` recovers `FmWindow *` via `XtNuserData` on the shell widget:
  ```c
  Arg a; XtSetArg(a, XtNuserData, NULL);
  XtGetValues(w, &a, 1);
  FmWindow *win = (FmWindow *)a.value;
  ```
- `clipboard_init` sets `XtNuserData` on `win->toplevel` to `win`.

**Test:** Build succeeds, copy/paste works.

### Step 1.6: Move DnD statics into FmWindow

**Files:** `dnd.c`, `fm.h`

- Remove `drag_fm`, `saved_press`, `press_valid`, `drag_paths`, `ndrag_paths`, `drop_was_noop`.
- Xt action procedures recover `FmWindow *` via `XtNuserData` on the iconview widget (set during `dnd_init`).
- `drag_convert`/`drag_finished`/`drop_cb` already receive client_data — cast to `FmWindow *`.

**Test:** Build succeeds, drag and drop works.

### Step 1.7: Move fm.c dialog/context/shortcut statics into FmWindow

**Files:** `fm.c`, `fm.h`

This is the largest step. Move all remaining fm.c statics into FmWindow:
- Dialog state: `rename_shell`, `rename_index`, `delete_shell`, `empty_trash_shell`
- Context menu: `ctx_shell`, `ctx_list`, `ctx_in_trash`, `dyn_labels`, `dyn_actions`, `dyn_nitems`
- Open-with: `ow_indices`, `ow_count`, `ow_label_buf`, `ow_file_path`
- Remove `shortcut_fm`.

**Xt action procedure pattern** — all `act_*` functions recover `FmWindow *` from widget:
```c
static FmWindow *win_from_widget(Widget w)
{
    /* Walk up to the nearest shell and read XtNuserData */
    while (w && !XtIsShell(w))
        w = XtParent(w);
    if (!w) return NULL;
    XtPointer ud = NULL;
    Arg a; XtSetArg(a, XtNuserData, &ud);
    XtGetValues(w, &a, 1);
    return (FmWindow *)ud;
}
```

For dialog dismiss actions (widget is the dialog shell), set `XtNuserData` on the dialog shell when creating it.

**Test:** Build succeeds. All shortcuts, context menu, rename/delete/trash dialogs work. Still single window.

---

## Phase 2: Multi-Window Support

### Step 2.1: Split fm_init into fm_app_init + fm_window_new

**Files:** `fm.c`, `fm.h`, `main.c`

- `fm_app_init(FmApp *app, int *argc, char **argv)`:
  - `XtAppInitialize` (creates first toplevel + display connection)
  - `icons_init(app)`
  - Desktop entry cache scan
  - D-Bus setup
  - `XtAppAddActions` (once, globally)
  - Calls `fm_window_new(app, initial_path)`

- `fm_window_new(FmApp *app, const char *path) → FmWindow *`:
  - Allocates FmWindow
  - First call: reuses `app->first_toplevel`. Subsequent: `XtAppCreateShell()`
  - Builds widget tree (MainWindow, vbox, navbar, hbox, places, fileview)
  - `clipboard_init`, `dnd_init`
  - `XtRealizeWidget`, navigate to path
  - Appends to `app->windows[]`

- `fm_window_destroy(FmWindow *win)`:
  - Destroys widget tree, frees per-window state
  - Removes from `app->windows[]`
  - If `app->nwindows == 0` → `app->running = 0`

- `main.c`:
  ```c
  FmApp app;
  fm_app_init(&app, &argc, argv);
  fm_app_run(&app);
  fm_app_cleanup(&app);
  ```

**Test:** Single window works. Can manually test `fm_window_new` from a shortcut.

### Step 2.2: "New Window" action

**Files:** `fm.c`, `fm.h`

- Add `act_new_window` → calls `fm_window_new(win->app_state, win->cwd)`.
- Add `Ctrl+N` to `fm_translations[]`.
- Add "New Window" to context menu base items.

**Test:** Ctrl+N opens second window. Close one → other stays. Close last → exits.

### Step 2.3: Multi-window clipboard ownership

**Files:** `clipboard.c`

- `app->clipboard_owner` tracks which window owns CLIPBOARD.
- `clipboard_paste` fast-paths only when `app->clipboard_owner == win`.
- `lose_selection` clears `app->clipboard_owner`.

**Test:** Copy in window A, paste in window B works.

---

## Phase 3: Single-Instance IPC

### Step 3.1: Instance detection via X selection

**Files:** new `instance.c` or in `fm.c`, `fm.h`, `CMakeLists.txt`

- Atom: `_ISDE_FM_INSTANCE`.
- On startup: `xcb_get_selection_owner` to check if owned.
- If not owned: `XtOwnSelection` on a helper window → we are the primary.
- If owned: send ClientMessage to owner with path, then exit.

**Test:** Second launch prints "forwarding to existing instance" and exits.

### Step 3.2: Open-path message handling

**Files:** `instance.c` or `fm.c`

- Sending side: sets `_ISDE_FM_OPEN_PATH` property on owner window with path string, sends ClientMessage notification.
- Receiving side: event handler reads property, calls `fm_window_new(app, path)`.

**Test:** `isde-fm /tmp` opens new window in running instance.

---

## Phase 4: Threaded File Operations

### Step 4.1: Worker thread infrastructure

**Files:** new `fileops_thread.c`, `fm.h`, `CMakeLists.txt`

```c
typedef enum { FM_JOB_COPY, FM_JOB_MOVE, FM_JOB_DELETE, FM_JOB_TRASH } FmJobType;

typedef struct FmJob {
    FmJobType    type;
    char       **src_paths;
    int          nsrc;
    char        *dst_dir;
    _Atomic int  files_done;
    _Atomic int  files_total;
    _Atomic int  cancelled;
    _Atomic int  finished;
    int          error;
    FmWindow    *origin_win;
    struct FmJob *next;
} FmJob;
```

- `FmApp` gets: `pthread_mutex_t job_mutex`, `pthread_cond_t job_cond`, `FmJob *job_head`, `pthread_t worker_thread`, `int notify_pipe[2]`.
- Worker thread: `lock → wait for job → unlock → execute → write byte to pipe`.
- `fm_app_init`: `pipe2(notify_pipe, O_CLOEXEC)`, `pthread_create`, `XtAppAddInput(read_end)`.
- Completion callback: reads byte, finds finished jobs, `fm_refresh(job->origin_win)`, frees job.

**Build:** Add `-lpthread` via `find_package(Threads)` + `target_link_libraries(isde-fm Threads::Threads)`.

**Test:** Build succeeds, thread starts, no visible change yet.

### Step 4.2: Migrate callers to job queue

**Files:** `fileops_thread.c`, `clipboard.c`, `dnd.c`, `fm.c`

- `fileops_submit_copy(app, win, srcs, nsrc, dst_dir) → FmJob *`
- Similarly: `_submit_move`, `_submit_delete`, `_submit_trash`.
- Worker calls existing `copy_recursive`/`delete_recursive` with cancellation checks.
- `clipboard.c do_file_op` → submits job instead of inline call.
- `dnd.c drop_cb` → submits copy job.
- `fm.c` delete/trash handlers → submit jobs.
- `fileops_mkdir`/`fileops_rename` stay synchronous (instant).
- `fm_refresh` calls move from callers into the job completion callback.

**Test:** Copy large directory → UI stays responsive. File appears when done.

### Step 4.3: Cancellation and error handling

**Files:** `fileops_thread.c`, `fileops.c`

- Check `job->cancelled` after each file in recursive loops.
- Store `job->error = errno` on first failure.
- Completion callback reports errors in status bar.

**Test:** Cancel mid-copy works. Permission denied shows error.

---

## Phase 5: Progress UI

### Step 5.1: Progress dialog

**Files:** new `progress.c`, `fm.h`, `CMakeLists.txt`

- Transient shell with Label + ProgressBar + Cancel button.
- 200ms timer polls `job->files_done` / `job->files_total` (atomic reads).
- Cancel button sets `job->cancelled = 1`.
- Auto-dismisses when `job->finished`.

**Test:** Large copy shows progress bar.

### Step 5.2: Show-delay threshold

**Files:** `progress.c`

- Start 500ms timer on job submission.
- If job finishes before timer → never show dialog.
- If still running → create dialog.

**Test:** Single file delete: no dialog flash. 1000-file copy: dialog after 500ms.

---

## Key Design Decisions

1. **FmWindow recovery from Xt actions:** `XtNuserData` on the nearest shell ancestor. Standard Xt pattern.

2. **Single worker thread:** File ops are I/O-bound, one at a time is fine. Queued if concurrent.

3. **Pipe notification:** `write(pipe, "x", 1)` from worker → `XtAppAddInput` fires on main thread. No Xt calls from worker.

4. **Atomic progress counters:** `_Atomic int` avoids locking for the timer-based progress poll.

5. **First shell from XtAppInitialize, rest from XtAppCreateShell:** Required by Xt bootstrap.

6. **Icons + desktop entries app-wide, places per-window:** Icons are expensive to load. Places are cheap widgets.

---

## Build System Changes

```cmake
# Top-level CMakeLists.txt
find_package(Threads REQUIRED)

# fm/CMakeLists.txt — add to source list:
#   src/fileops_thread.c
#   src/progress.c
# Add to link libraries:
#   Threads::Threads
```
