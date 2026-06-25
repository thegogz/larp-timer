#include <pebble.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define MAX_TIMERS       5
#define TIMER_NAME_LEN   9   // 8 chars + null
#define TAP_WINDOW_MS    2000
#define FLASH_DURATION_MS 2000
#define TICK_MS          1000

typedef enum {
  PKEY_TIMER_CONFIGS = 1,
  PKEY_SELECTED_ROW  = 2,
} PersistKey;

// ============================================================================
// DATA TYPES
// ============================================================================

typedef enum {
  INTERVAL_HOURLY  = 0,
  INTERVAL_MINUTES = 1,
  INTERVAL_SECONDS = 2,
} IntervalType;

typedef enum {
  VIBE_SINGLE          = 0,
  VIBE_DOUBLE          = 1,
  VIBE_TRIPLE          = 2,
  VIBE_LONG            = 3,
  VIBE_LONG_SHORT      = 4,
  VIBE_SHORT_LONG_SHORT = 5,
  NUM_VIBE_TYPES       = 6,
} VibePatternType;

typedef struct {
  bool         configured;
  uint8_t      interval_type;
  uint16_t     interval_value;
  uint8_t      vibe_type;
  char         name[TIMER_NAME_LEN];
} TimerConfig;

// Runtime-only state (not persisted)
typedef struct {
  bool    running;
  int32_t remaining_secs;
} TimerState;

// ============================================================================
// VIBRATION PATTERNS
// ============================================================================

static const uint32_t s_vibe_seg1[] = {200};
static const uint32_t s_vibe_seg2[] = {150, 100, 150};
static const uint32_t s_vibe_seg3[] = {100,  80, 100,  80, 100};
static const uint32_t s_vibe_seg4[] = {700};
static const uint32_t s_vibe_seg5[] = {700, 150, 200};
static const uint32_t s_vibe_seg6[] = {150, 80, 700, 80, 150};

static const uint32_t *s_vibe_durations[NUM_VIBE_TYPES] = {
  s_vibe_seg1, s_vibe_seg2, s_vibe_seg3,
  s_vibe_seg4, s_vibe_seg5, s_vibe_seg6,
};
static const uint8_t s_vibe_num_segs[NUM_VIBE_TYPES] = {1, 3, 5, 1, 3, 5};

static const char *s_vibe_names[NUM_VIBE_TYPES] = {
  "Single  (short)",
  "Double  (short)",
  "Triple  (short)",
  "Long    (buzz)",
  "Long-Short",
  "Shrt-Long-Shrt",
};
static const char *s_vibe_symbols[NUM_VIBE_TYPES] = {
  ".", "..", "...", "-", "-.", ".-.",
};

// Preset names for the config name-picker
static const char * const s_preset_names[] = {
  "Spell",  "Potion", "Shield", "Stun",   "Heal",
  "Ward",   "Rage",   "Curse",  "Trap",   "Move",
  "Burn",   "Freeze", "Drain",  "Boost",  "Timer",
  "T1",     "T2",     "T3",     "T4",     "T5",
};
#define NUM_PRESET_NAMES 20

// Per-timer flash colors (color platforms)
static const GColor s_timer_colors[MAX_TIMERS] = {
  {.argb = 0b11110000},  // Red     (~GColorRed)
  {.argb = 0b11000011},  // Blue    (~GColorBlue)
  {.argb = 0b11001100},  // Green   (~GColorGreen)
  {.argb = 0b11111100},  // Yellow  (~GColorYellow)
  {.argb = 0b11110011},  // Magenta (~GColorMagenta)
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

static TimerConfig s_timer_configs[MAX_TIMERS];
static TimerState  s_timer_states[MAX_TIMERS];

// Seconds tick
static AppTimer *s_tick_timer = NULL;

// Tap-to-start interface
static bool       s_tap_armed  = false;
static int        s_tap_count  = 0;
static AppTimer  *s_tap_timer  = NULL;

// Flash overlay
static Window    *s_flash_window    = NULL;
static Layer     *s_flash_layer     = NULL;
static AppTimer  *s_flash_dismiss   = NULL;
static int        s_flash_idx       = -1;

// Main window
static Window         *s_main_window = NULL;
static MenuLayer      *s_menu_layer  = NULL;

// Config state
static int s_cfg_idx = 0;  // which timer is being configured

static Window         *s_cfg_type_window = NULL;
static SimpleMenuLayer *s_cfg_type_menu  = NULL;

static NumberWindow   *s_cfg_num_window  = NULL;

static Window         *s_cfg_vibe_window = NULL;
static SimpleMenuLayer *s_cfg_vibe_menu  = NULL;

static Window          *s_cfg_name_window = NULL;
static SimpleMenuLayer *s_cfg_name_menu   = NULL;
static SimpleMenuItem   s_cfg_name_items[NUM_PRESET_NAMES];
static SimpleMenuSection s_cfg_name_section;

// Tap indicator string and layer (shown on main screen)
static char  s_tap_label[16]         = "";
static Layer *s_tap_indicator_layer  = NULL;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void tick_callback(void *data);
static void push_config_menu(int timer_idx);
static void push_config_name(void);
static void push_config_type(void);
static void push_config_vibe(void);

// ============================================================================
// UTILITIES
// ============================================================================

static int32_t get_total_secs(int idx) {
  switch (s_timer_configs[idx].interval_type) {
    case INTERVAL_HOURLY:  return 3600;
    case INTERVAL_MINUTES: return (int32_t)s_timer_configs[idx].interval_value * 60;
    case INTERVAL_SECONDS: return (int32_t)s_timer_configs[idx].interval_value;
    default:               return 3600;
  }
}

static void format_countdown(char *buf, size_t sz, int32_t secs) {
  if (secs < 0) secs = 0;
  if (secs >= 3600) {
    snprintf(buf, sz, "%dh%02dm", (int)(secs / 3600), (int)((secs % 3600) / 60));
  } else {
    snprintf(buf, sz, "%02d:%02d", (int)(secs / 60), (int)(secs % 60));
  }
}

static void format_interval(char *buf, size_t sz, int idx) {
  TimerConfig *c = &s_timer_configs[idx];
  switch (c->interval_type) {
    case INTERVAL_HOURLY:
      snprintf(buf, sz, "Hourly  [%s]", s_vibe_symbols[c->vibe_type]);
      break;
    case INTERVAL_MINUTES:
      snprintf(buf, sz, "Every %dmin [%s]", c->interval_value, s_vibe_symbols[c->vibe_type]);
      break;
    case INTERVAL_SECONDS:
      snprintf(buf, sz, "Every %dsec [%s]", c->interval_value, s_vibe_symbols[c->vibe_type]);
      break;
  }
}

static void do_vibrate(int idx) {
  VibePattern pat = {
    .durations    = s_vibe_durations[s_timer_configs[idx].vibe_type],
    .num_segments = s_vibe_num_segs[s_timer_configs[idx].vibe_type],
  };
  vibes_enqueue_custom_pattern(pat);
}

// ============================================================================
// PERSISTENCE
// ============================================================================

static void save_configs(void) {
  persist_write_data(PKEY_TIMER_CONFIGS, s_timer_configs, sizeof(s_timer_configs));
}

static void load_configs(void) {
  // Set up defaults first
  const char *default_names[MAX_TIMERS] = {"T1", "T2", "T3", "T4", "T5"};
  for (int i = 0; i < MAX_TIMERS; i++) {
    s_timer_configs[i].configured    = false;
    s_timer_configs[i].interval_type = INTERVAL_MINUTES;
    s_timer_configs[i].interval_value = 5;
    s_timer_configs[i].vibe_type     = (uint8_t)i;
    snprintf(s_timer_configs[i].name, TIMER_NAME_LEN, "%s", default_names[i]);
  }

  if (persist_exists(PKEY_TIMER_CONFIGS) &&
      persist_get_size(PKEY_TIMER_CONFIGS) == (int)sizeof(s_timer_configs)) {
    persist_read_data(PKEY_TIMER_CONFIGS, s_timer_configs, sizeof(s_timer_configs));
  }
}

// ============================================================================
// TIMER FIRE / START / STOP
// ============================================================================

static void flash_dismiss_cb(void *data);

static void fire_timer(int idx) {
  do_vibrate(idx);
  s_timer_states[idx].remaining_secs = get_total_secs(idx);

  if (s_flash_window && window_stack_contains_window(s_flash_window)) {
    // Flash visible — update to new timer only if a different one fired
    if (s_flash_idx != idx) {
      s_flash_idx = idx;
      layer_mark_dirty(s_flash_layer);
      if (s_flash_dismiss) app_timer_reschedule(s_flash_dismiss, FLASH_DURATION_MS);
    }
    // Same timer refiring: let the existing dismiss timer run — no extension
  } else {
    s_flash_idx = idx;
    extern void push_flash_window(int timer_idx);
    push_flash_window(idx);
  }
}

static void ensure_tick_running(void) {
  if (s_tick_timer == NULL) {
    s_tick_timer = app_timer_register(TICK_MS, tick_callback, NULL);
  }
}

static void start_timer(int idx) {
  if (!s_timer_configs[idx].configured) return;
  s_timer_states[idx].running       = true;
  s_timer_states[idx].remaining_secs = get_total_secs(idx);
  ensure_tick_running();
  if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static void stop_timer(int idx) {
  s_timer_states[idx].running = false;
  if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static void toggle_timer(int idx) {
  if (idx < 0 || idx >= MAX_TIMERS) return;
  if (!s_timer_configs[idx].configured) {
    push_config_menu(idx);
    return;
  }
  if (s_timer_states[idx].running) {
    stop_timer(idx);
  } else {
    start_timer(idx);
  }
}

// ============================================================================
// TICK CALLBACK  (fires every second)
// ============================================================================

static void tick_callback(void *data) {
  s_tick_timer = NULL;
  bool any_running = false;

  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!s_timer_states[i].running) continue;
    any_running = true;
    s_timer_states[i].remaining_secs--;
    if (s_timer_states[i].remaining_secs <= 0) {
      fire_timer(i);
    }
  }

  if (any_running) {
    s_tick_timer = app_timer_register(TICK_MS, tick_callback, NULL);
    if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
  }
}

// ============================================================================
// FLASH WINDOW
// ============================================================================

static void flash_layer_update(Layer *layer, GContext *ctx) {
  if (s_flash_idx < 0 || s_flash_idx >= MAX_TIMERS) return;

  GRect bounds = layer_get_bounds(layer);
  TimerConfig *cfg = &s_timer_configs[s_flash_idx];

  // Background fill (color-coded per timer)
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, s_timer_colors[s_flash_idx]);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Large timer name
  GFont font_big  = fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD);
  GFont font_med  = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont font_sm   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif

  // Timer name centered
  GRect name_box = GRect(0, 40, bounds.size.w, 60);
  graphics_draw_text(ctx, cfg->name, font_big, name_box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // "FIRED!" label
  GRect fired_box = GRect(0, 100, bounds.size.w, 40);
  graphics_draw_text(ctx, "FIRED!", font_med, fired_box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Interval info
  char interval_buf[32];
  format_interval(interval_buf, sizeof(interval_buf), s_flash_idx);
  GRect info_box = GRect(4, 148, bounds.size.w - 8, 28);
  graphics_draw_text(ctx, interval_buf, font_sm, info_box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void flash_dismiss_cb(void *data) {
  s_flash_dismiss = NULL;
  if (s_flash_window && window_stack_contains_window(s_flash_window)) {
    window_stack_pop(false);
  }
}

static void flash_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_flash_layer = layer_create(bounds);
  layer_set_update_proc(s_flash_layer, flash_layer_update);
  layer_add_child(root, s_flash_layer);
  s_flash_dismiss = app_timer_register(FLASH_DURATION_MS, flash_dismiss_cb, NULL);
}

static void flash_window_unload(Window *window) {
  if (s_flash_dismiss) {
    app_timer_cancel(s_flash_dismiss);
    s_flash_dismiss = NULL;
  }
  layer_destroy(s_flash_layer);
  s_flash_layer = NULL;
  window_destroy(window);
  s_flash_window = NULL;
}

void push_flash_window(int timer_idx) {
  s_flash_idx    = timer_idx;
  s_flash_window = window_create();
  window_set_background_color(s_flash_window, GColorBlack);
  window_set_window_handlers(s_flash_window, (WindowHandlers){
    .load   = flash_window_load,
    .unload = flash_window_unload,
  });
  window_stack_push(s_flash_window, false);
}

// ============================================================================
// CONFIG — VIBE SELECTION WINDOW
// ============================================================================

static SimpleMenuItem s_cfg_vibe_items[NUM_VIBE_TYPES];
static SimpleMenuSection s_cfg_vibe_section;

static void cfg_vibe_select(int index, void *ctx) {
  s_timer_configs[s_cfg_idx].vibe_type = (uint8_t)index;
  save_configs();
  vibes_short_pulse();
  window_stack_pop(false);  // pop vibe → back to config menu
}

static void cfg_vibe_window_load(Window *window) {
  for (int i = 0; i < NUM_VIBE_TYPES; i++) {
    s_cfg_vibe_items[i].title    = s_vibe_names[i];
    s_cfg_vibe_items[i].subtitle = s_vibe_symbols[i];
    s_cfg_vibe_items[i].callback = cfg_vibe_select;
  }
  s_cfg_vibe_section.title     = "Vibration";
  s_cfg_vibe_section.items     = s_cfg_vibe_items;
  s_cfg_vibe_section.num_items = NUM_VIBE_TYPES;

  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);
  s_cfg_vibe_menu = simple_menu_layer_create(bounds, window,
                                              &s_cfg_vibe_section, 1, NULL);
  MenuLayer *ml = simple_menu_layer_get_menu_layer(s_cfg_vibe_menu);
  MenuIndex mi  = {.section = 0, .row = s_timer_configs[s_cfg_idx].vibe_type};
  menu_layer_set_selected_index(ml, mi, MenuRowAlignCenter, false);
  layer_add_child(root, simple_menu_layer_get_layer(s_cfg_vibe_menu));
}

static void cfg_vibe_window_unload(Window *window) {
  simple_menu_layer_destroy(s_cfg_vibe_menu);
  s_cfg_vibe_menu = NULL;
  window_destroy(window);
  s_cfg_vibe_window = NULL;
}

static void push_config_vibe(void) {
  s_cfg_vibe_window = window_create();
  window_set_window_handlers(s_cfg_vibe_window, (WindowHandlers){
    .load   = cfg_vibe_window_load,
    .unload = cfg_vibe_window_unload,
  });
  window_stack_push(s_cfg_vibe_window, true);
}

// ============================================================================
// CONFIG — VALUE (NUMBER) WINDOW
// ============================================================================

static void cfg_num_selected(struct NumberWindow *nw, void *ctx) {
  s_timer_configs[s_cfg_idx].interval_value = (uint16_t)number_window_get_value(nw);
  s_timer_configs[s_cfg_idx].configured = true;
  save_configs();
  vibes_short_pulse();
  window_stack_pop(false);  // pop number
  window_stack_pop(false);  // pop type → back to config menu
}

static void push_config_number(void) {
  TimerConfig *cfg = &s_timer_configs[s_cfg_idx];
  const char *label;
  int32_t min_val, max_val, step, current;

  if (cfg->interval_type == INTERVAL_MINUTES) {
    label   = "Minutes (1-120)";
    min_val = 1; max_val = 120; step = 1;
    current = (cfg->interval_value > 0) ? cfg->interval_value : 5;
  } else {
    label   = "Seconds (10-60)";
    min_val = 10; max_val = 60; step = 10;
    current = (cfg->interval_value >= 10) ? (cfg->interval_value / 10) * 10 : 10;
  }

  s_cfg_num_window = number_window_create(label,
    (NumberWindowCallbacks){ .selected = cfg_num_selected }, NULL);
  number_window_set_min(s_cfg_num_window, min_val);
  number_window_set_max(s_cfg_num_window, max_val);
  number_window_set_step_size(s_cfg_num_window, step);
  number_window_set_value(s_cfg_num_window, current);
  window_stack_push(number_window_get_window(s_cfg_num_window), true);
}

// ============================================================================
// CONFIG — TYPE SELECTION WINDOW
// ============================================================================

static SimpleMenuItem s_cfg_type_items[3];
static SimpleMenuSection s_cfg_type_section;

static void cfg_type_select(int index, void *ctx) {
  s_timer_configs[s_cfg_idx].interval_type = (uint8_t)index;
  if (index == INTERVAL_HOURLY) {
    s_timer_configs[s_cfg_idx].interval_value = 0;
    s_timer_configs[s_cfg_idx].configured = true;
    save_configs();
    vibes_short_pulse();
    window_stack_pop(false);  // pop type → back to config menu
  } else {
    push_config_number();
  }
}

static void cfg_type_window_load(Window *window) {
  s_cfg_type_items[0].title    = "Hourly";
  s_cfg_type_items[0].subtitle = "Every 60 minutes";
  s_cfg_type_items[0].callback = cfg_type_select;
  s_cfg_type_items[1].title    = "Every X Minutes";
  s_cfg_type_items[1].subtitle = "1 - 120 minutes";
  s_cfg_type_items[1].callback = cfg_type_select;
  s_cfg_type_items[2].title    = "Every X Seconds";
  s_cfg_type_items[2].subtitle = "10 - 60 seconds";
  s_cfg_type_items[2].callback = cfg_type_select;

  s_cfg_type_section.title     = s_timer_configs[s_cfg_idx].name;
  s_cfg_type_section.items     = s_cfg_type_items;
  s_cfg_type_section.num_items = 3;

  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);
  s_cfg_type_menu = simple_menu_layer_create(bounds, window,
                                              &s_cfg_type_section, 1, NULL);
  MenuLayer *ml = simple_menu_layer_get_menu_layer(s_cfg_type_menu);
  MenuIndex mi  = {.section = 0, .row = s_timer_configs[s_cfg_idx].interval_type};
  menu_layer_set_selected_index(ml, mi, MenuRowAlignCenter, false);
  layer_add_child(root, simple_menu_layer_get_layer(s_cfg_type_menu));
}

static void cfg_type_window_unload(Window *window) {
  simple_menu_layer_destroy(s_cfg_type_menu);
  s_cfg_type_menu = NULL;
  window_destroy(window);
  s_cfg_type_window = NULL;
}

static void push_config_type(void) {
  s_cfg_type_window = window_create();
  window_set_window_handlers(s_cfg_type_window, (WindowHandlers){
    .load   = cfg_type_window_load,
    .unload = cfg_type_window_unload,
  });
  window_stack_push(s_cfg_type_window, true);
}

// ============================================================================
// CONFIG — NAME SELECTION WINDOW
// ============================================================================

static void cfg_name_select(int index, void *ctx) {
  snprintf(s_timer_configs[s_cfg_idx].name, TIMER_NAME_LEN, "%s", s_preset_names[index]);
  save_configs();
  vibes_short_pulse();
  window_stack_pop(false);  // pop name → back to config menu
}

static void cfg_name_window_load(Window *window) {
  for (int i = 0; i < NUM_PRESET_NAMES; i++) {
    s_cfg_name_items[i].title    = s_preset_names[i];
    s_cfg_name_items[i].callback = cfg_name_select;
  }
  s_cfg_name_section.title     = "Timer Name";
  s_cfg_name_section.items     = s_cfg_name_items;
  s_cfg_name_section.num_items = NUM_PRESET_NAMES;

  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);
  s_cfg_name_menu = simple_menu_layer_create(bounds, window,
                                              &s_cfg_name_section, 1, NULL);
  MenuLayer *ml = simple_menu_layer_get_menu_layer(s_cfg_name_menu);
  for (int i = 0; i < NUM_PRESET_NAMES; i++) {
    if (strcmp(s_timer_configs[s_cfg_idx].name, s_preset_names[i]) == 0) {
      MenuIndex mi = {.section = 0, .row = (uint16_t)i};
      menu_layer_set_selected_index(ml, mi, MenuRowAlignCenter, false);
      break;
    }
  }
  layer_add_child(root, simple_menu_layer_get_layer(s_cfg_name_menu));
}

static void cfg_name_window_unload(Window *window) {
  simple_menu_layer_destroy(s_cfg_name_menu);
  s_cfg_name_menu = NULL;
  window_destroy(window);
  s_cfg_name_window = NULL;
}

static void push_config_name(void) {
  s_cfg_name_window = window_create();
  window_set_window_handlers(s_cfg_name_window, (WindowHandlers){
    .load   = cfg_name_window_load,
    .unload = cfg_name_window_unload,
  });
  window_stack_push(s_cfg_name_window, true);
}

// ============================================================================
// CONFIG — EDIT MENU  (Name / Time / Vibration independently)
// ============================================================================

static Window          *s_cfg_edit_window = NULL;
static SimpleMenuLayer *s_cfg_edit_menu   = NULL;
static SimpleMenuItem   s_cfg_edit_items[3];
static SimpleMenuSection s_cfg_edit_section;
static char s_cfg_edit_sub_time[32];

static void cfg_edit_update_subtitles(void) {
  TimerConfig *cfg = &s_timer_configs[s_cfg_idx];
  switch (cfg->interval_type) {
    case INTERVAL_HOURLY:
      snprintf(s_cfg_edit_sub_time, sizeof(s_cfg_edit_sub_time), "Hourly");
      break;
    case INTERVAL_MINUTES:
      snprintf(s_cfg_edit_sub_time, sizeof(s_cfg_edit_sub_time), "Every %d min", cfg->interval_value);
      break;
    default:
      snprintf(s_cfg_edit_sub_time, sizeof(s_cfg_edit_sub_time), "Every %d sec", cfg->interval_value);
      break;
  }
}

static void cfg_edit_item_selected(int index, void *ctx) {
  switch (index) {
    case 0: push_config_name(); break;
    case 1: push_config_type(); break;
    case 2: push_config_vibe(); break;
  }
}

static void cfg_edit_window_appear(Window *window) {
  cfg_edit_update_subtitles();
  if (s_cfg_edit_menu) layer_mark_dirty(simple_menu_layer_get_layer(s_cfg_edit_menu));
  if (s_menu_layer)    layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static void cfg_edit_window_load(Window *window) {
  cfg_edit_update_subtitles();

  s_cfg_edit_items[0].title    = "Name";
  s_cfg_edit_items[0].subtitle = s_timer_configs[s_cfg_idx].name;
  s_cfg_edit_items[0].callback = cfg_edit_item_selected;
  s_cfg_edit_items[1].title    = "Time";
  s_cfg_edit_items[1].subtitle = s_cfg_edit_sub_time;
  s_cfg_edit_items[1].callback = cfg_edit_item_selected;
  s_cfg_edit_items[2].title    = "Vibration";
  s_cfg_edit_items[2].subtitle = s_vibe_names[s_timer_configs[s_cfg_idx].vibe_type];
  s_cfg_edit_items[2].callback = cfg_edit_item_selected;

  s_cfg_edit_section.title     = s_timer_configs[s_cfg_idx].name;
  s_cfg_edit_section.items     = s_cfg_edit_items;
  s_cfg_edit_section.num_items = 3;

  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);
  s_cfg_edit_menu = simple_menu_layer_create(bounds, window,
                                              &s_cfg_edit_section, 1, NULL);
  layer_add_child(root, simple_menu_layer_get_layer(s_cfg_edit_menu));
}

static void cfg_edit_window_unload(Window *window) {
  simple_menu_layer_destroy(s_cfg_edit_menu);
  s_cfg_edit_menu = NULL;
  window_destroy(window);
  s_cfg_edit_window = NULL;
  if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static void push_config_menu(int timer_idx) {
  s_cfg_idx = timer_idx;
  s_cfg_edit_window = window_create();
  window_set_window_handlers(s_cfg_edit_window, (WindowHandlers){
    .load   = cfg_edit_window_load,
    .appear = cfg_edit_window_appear,
    .unload = cfg_edit_window_unload,
  });
  window_stack_push(s_cfg_edit_window, true);
}

// ============================================================================
// ACTION MENU (Start/Stop + Configure)
// ============================================================================

static void action_start_stop(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(uintptr_t)action_menu_item_get_action_data(item);
  toggle_timer(idx);
}

static void action_configure(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  int idx = (int)(uintptr_t)action_menu_item_get_action_data(item);
  push_config_menu(idx);
}

static void action_menu_did_close(ActionMenu *am, const ActionMenuItem *item, void *ctx) {
  ActionMenuLevel *root = action_menu_get_root_level(am);
  action_menu_hierarchy_destroy(root, NULL, NULL);
}

static void open_action_menu(int idx) {
  bool is_running = s_timer_states[idx].running;
  const char *toggle_label = is_running ? "Stop Timer" : "Start Timer";

  ActionMenuLevel *root = action_menu_level_create(2);
  action_menu_level_add_action(root, toggle_label, action_start_stop,
                               (void *)(uintptr_t)idx);
  action_menu_level_add_action(root, "Configure",  action_configure,
                               (void *)(uintptr_t)idx);

  ActionMenuConfig cfg = {
    .root_level   = root,
    .did_close    = action_menu_did_close,
    .align        = ActionMenuAlignTop,
#ifdef PBL_COLOR
    .colors.background = GColorDarkGray,
#endif
  };
  action_menu_open(&cfg);
}

// ============================================================================
// TAP HANDLER (accelerometer wrist-tap interface)
// ============================================================================

static void tap_timeout_cb(void *data) {
  s_tap_timer = NULL;
  int count   = s_tap_count;
  s_tap_armed = false;
  s_tap_count = 0;
  s_tap_label[0] = '\0';

  if (count >= 1 && count <= MAX_TIMERS) {
    toggle_timer(count - 1);  // 1 tap = timer 0, 2 taps = timer 1, ...
  }

  if (s_tap_indicator_layer) layer_mark_dirty(s_tap_indicator_layer);
  if (s_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  if (!s_tap_armed) {
    s_tap_armed = true;
    s_tap_count = 0;
    snprintf(s_tap_label, sizeof(s_tap_label), "TAP: -");
    s_tap_timer = app_timer_register(TAP_WINDOW_MS, tap_timeout_cb, NULL);
  } else {
    s_tap_count++;
    if (s_tap_count > MAX_TIMERS) s_tap_count = MAX_TIMERS;
    snprintf(s_tap_label, sizeof(s_tap_label), "TAP: T%d", s_tap_count);
    if (s_tap_timer) app_timer_reschedule(s_tap_timer, TAP_WINDOW_MS);
  }
  if (s_tap_indicator_layer) layer_mark_dirty(s_tap_indicator_layer);
}

// ============================================================================
// MAIN WINDOW — MenuLayer
// ============================================================================

static uint16_t menu_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return MAX_TIMERS;
}

static int16_t menu_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  // 40px rows → all 5 timers visible simultaneously without scrolling
  return 40;
}

static void menu_draw_row(GContext *gctx, const Layer *cell_layer,
                          MenuIndex *idx, void *ctx) {
  int i = idx->row;
  TimerConfig *cfg   = &s_timer_configs[i];
  TimerState  *state = &s_timer_states[i];

  GRect bounds  = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_menu_layer, idx);
  int mid_y     = bounds.size.h / 2;  // vertical centre = 20

  GColor bg  = selected ? PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack) : GColorWhite;
  GColor fg  = selected ? GColorWhite : GColorBlack;
  GColor dim = selected ? GColorWhite : PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);

  graphics_context_set_fill_color(gctx, bg);
  graphics_fill_rect(gctx, bounds, 0, GCornerNone);

  GFont font_main = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont font_tiny = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  // ── Name (left, large) ──────────────────────────────────────
  graphics_context_set_text_color(gctx, cfg->configured ? fg : dim);
  graphics_draw_text(gctx, cfg->name, font_main,
                     GRect(4, 4, 84, 32),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // ── State icon (centred at x=97, y=mid_y) ────────────────────
  // Icon area: x=90 to x=104 (14px), vertically centred
  if (!cfg->configured) {
    graphics_context_set_stroke_color(gctx, dim);
    graphics_context_set_stroke_width(gctx, 2);
    graphics_draw_circle(gctx, GPoint(97, mid_y), 6);
    graphics_context_set_stroke_width(gctx, 1);
  } else if (state->running) {
    GColor ic = selected ? GColorWhite : PBL_IF_COLOR_ELSE(s_timer_colors[i], GColorBlack);
    graphics_context_set_fill_color(gctx, ic);
    GPoint pts[3] = {
      GPoint(90, mid_y - 7),
      GPoint(90, mid_y + 7),
      GPoint(104, mid_y),
    };
    GPathInfo pi = {.num_points = 3, .points = pts};
    GPath *play = gpath_create(&pi);
    gpath_draw_filled(gctx, play);
    gpath_destroy(play);
  } else {
    graphics_context_set_fill_color(gctx, dim);
    graphics_fill_rect(gctx, GRect(91, mid_y - 6, 12, 12), 0, GCornerNone);
  }

  // ── Countdown (centre-right, large) ─────────────────────────
  if (cfg->configured) {
    char countdown[16];
    if (state->running) {
      format_countdown(countdown, sizeof(countdown), state->remaining_secs);
      graphics_context_set_text_color(gctx, fg);
    } else {
      snprintf(countdown, sizeof(countdown), "--:--");
      graphics_context_set_text_color(gctx, dim);
    }
    graphics_draw_text(gctx, countdown, font_main,
                       GRect(108, 4, 62, 32),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    // ── Interval brief (far right, small) ───────────────────────
    char brief[8];
    switch (cfg->interval_type) {
      case INTERVAL_HOURLY:  snprintf(brief, sizeof(brief), "1hr");             break;
      case INTERVAL_MINUTES: snprintf(brief, sizeof(brief), "%dm", cfg->interval_value); break;
      default:               snprintf(brief, sizeof(brief), "%ds", cfg->interval_value); break;
    }
    graphics_context_set_text_color(gctx, dim);
    graphics_draw_text(gctx, brief, font_tiny,
                       GRect(174, 14, 22, 14),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

static void menu_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  open_action_menu(idx->row);
}

static void menu_long_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  toggle_timer(idx->row);
  // Short double-pulse confirms the long-press toggle
  VibePattern confirm = {.durations = (uint32_t[]){50, 80, 50}, .num_segments = 3};
  vibes_enqueue_custom_pattern(confirm);
}

// ============================================================================
// MAIN WINDOW — TAP INDICATOR LAYER
// ============================================================================

static void tap_indicator_update(Layer *layer, GContext *ctx) {
  if (!s_tap_armed || s_tap_label[0] == '\0') return;

  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx,
    PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack));
  graphics_fill_rect(ctx, bounds, 4, GCornersAll);

  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GRect text_rect = GRect(0, 2, bounds.size.w, bounds.size.h - 2);
  graphics_draw_text(ctx, s_tap_label, font, text_rect,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// ============================================================================
// MAIN WINDOW LIFECYCLE
// ============================================================================

static void main_window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  // MenuLayer: takes up most of the screen, leave bottom 24px for tap indicator
  GRect menu_frame = GRect(0, 0, bounds.size.w, bounds.size.h - 24);
  s_menu_layer = menu_layer_create(menu_frame);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows   = menu_num_rows,
    .get_cell_height = menu_cell_height,
    .draw_row       = menu_draw_row,
    .select_click      = menu_select,
    .select_long_click = menu_long_select,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  menu_layer_set_normal_colors(s_menu_layer, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_menu_layer,
    PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack),
    GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));

  // Tap indicator bar at the bottom
  GRect tap_frame = GRect(0, bounds.size.h - 24, bounds.size.w, 24);
  s_tap_indicator_layer = layer_create(tap_frame);
  layer_set_update_proc(s_tap_indicator_layer, tap_indicator_update);
  layer_add_child(root, s_tap_indicator_layer);

  // Restore last selected row
  if (persist_exists(PKEY_SELECTED_ROW)) {
    int last = persist_read_int(PKEY_SELECTED_ROW);
    if (last >= 0 && last < MAX_TIMERS) {
      MenuIndex mi = {.section = 0, .row = (uint16_t)last};
      menu_layer_set_selected_index(s_menu_layer, mi, MenuRowAlignCenter, false);
    }
  }
}

static void main_window_unload(Window *window) {
  // Save selected row
  MenuIndex mi = menu_layer_get_selected_index(s_menu_layer);
  persist_write_int(PKEY_SELECTED_ROW, mi.row);

  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
  layer_destroy(s_tap_indicator_layer);
  s_tap_indicator_layer = NULL;
}

static void main_window_appear(Window *window) {
  accel_tap_service_subscribe(accel_tap_handler);
}

static void main_window_disappear(Window *window) {
  accel_tap_service_unsubscribe();
}

// ============================================================================
// APP LIFECYCLE
// ============================================================================

static void init(void) {
  load_configs();

  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorWhite);
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load      = main_window_load,
    .unload    = main_window_unload,
    .appear    = main_window_appear,
    .disappear = main_window_disappear,
  });
  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  // Cancel active timers
  if (s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_timer = NULL;
  }
  if (s_tap_timer) {
    app_timer_cancel(s_tap_timer);
    s_tap_timer = NULL;
  }
  window_destroy(s_main_window);
  s_main_window = NULL;
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
