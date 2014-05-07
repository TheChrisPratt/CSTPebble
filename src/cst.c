/*
   Big Time watch using the Central Standard Time Font

   A digital watch with large, fluid digits.


   A few things complicate the implementation of this watch:

   a) The fact that I don't actually have the CST font requires us 
      to generate and use images instead of fonts -- which 
      complicates things greatly.

   b) When I started it wasn't possible to load all the images into
      RAM at once -- this means we have to load/unload each image when
      we need it. The images are slightly smaller now than they were
      but I figured it would still be pushing it to load them all at
      once, even if they just fit, so I've stuck with the load/unload
      approach.
 */
#include "pebble.h"

static Window *window;

//
// There's only enough memory to load about 6 of 10 required images
// so we have to swap them in & out...
//
// We have one "slot" per digit location on screen.
//
// Because layers can only have one parent we load a digit for each
// slot--even if the digit image is already in another slot.
//
// Slot on-screen layout:
//     0 1
//     2 3
//
#define TOTAL_IMAGE_SLOTS 4

#define NUMBER_OF_IMAGES 10
#define NUMBER_OF_POWER_IMAGES 6

// These images are 72 x 74 pixels (i.e. a quarter of the display),
// black and white with the digit character centered in the image.
const int IMAGE_RESOURCE_IDS[NUMBER_OF_IMAGES] = {
  RESOURCE_ID_IMAGE_NUM_0, RESOURCE_ID_IMAGE_NUM_1, RESOURCE_ID_IMAGE_NUM_2,
  RESOURCE_ID_IMAGE_NUM_3, RESOURCE_ID_IMAGE_NUM_4, RESOURCE_ID_IMAGE_NUM_5,
  RESOURCE_ID_IMAGE_NUM_6, RESOURCE_ID_IMAGE_NUM_7, RESOURCE_ID_IMAGE_NUM_8,
  RESOURCE_ID_IMAGE_NUM_9
};

const int POWER_IMAGE_RESOURCE_IDS[NUMBER_OF_POWER_IMAGES] = {
  RESOURCE_ID_IMAGE_POWER_0, RESOURCE_ID_IMAGE_POWER_1, RESOURCE_ID_IMAGE_POWER_2,
  RESOURCE_ID_IMAGE_POWER_3, RESOURCE_ID_IMAGE_POWER_4, RESOURCE_ID_IMAGE_POWER_5
};

enum SettingsKeys {
  ZERO_PREFIX = 0x00, // boolean (6 bytes = 6)
  SHOW_POWER  = 0x01, // boolean (6 bytes = 12)
  SHOW_BTOOTH = 0x02, // boolean (6 bytes = 18)
  MONTH_FIRST = 0x03, // boolean (6 bytes = 24)
  SUN_TEXT    = 0x04, // string (4 bytes = 28)
  MON_TEXT    = 0x05, // string (4 bytes = 32)
  TUE_TEXT    = 0x06, // string (4 bytes = 36)
  WED_TEXT    = 0x07, // string (4 bytes = 40)
  THU_TEXT    = 0x08, // string (4 bytes = 44)
  FRI_TEXT    = 0x09, // string (4 bytes = 48)
  SAT_TEXT    = 0x0A, // string (4 bytes = 52)
  VIBE_HOUR   = 0x0B, // boolean (6 bytes = 58)
  VIBE_BTOOTH = 0x0C, // boolean (6 bytes = 64)
};

static AppSync sync;
static uint8_t sync_buffer[256];

static GBitmap *images[TOTAL_IMAGE_SLOTS];
static BitmapLayer *image_layers[TOTAL_IMAGE_SLOTS];
static GBitmap *bluetooth_image = NULL;
static BitmapLayer *bluetooth_layer = NULL;
static GBitmap *power_image = NULL;
static BitmapLayer *power_layer = NULL;
static TextLayer *text_layer = NULL;
static const uint32_t const asc_segments[] = { 200, 100, 400 };
static const uint32_t const desc_segments[] = { 400, 100, 200 };
static VibePattern asc = {
  .durations = asc_segments,
  .num_segments = ARRAY_LENGTH(asc_segments)
};
static VibePattern desc = {
  .durations = desc_segments,
  .num_segments = ARRAY_LENGTH(desc_segments)
};

#define EMPTY_SLOT -1

// The state is either "empty" or the digit of the image currently in
// the slot
static int image_slot_state[TOTAL_IMAGE_SLOTS] = {EMPTY_SLOT, EMPTY_SLOT, EMPTY_SLOT, EMPTY_SLOT};
static bool prev_bluetooth = false;
static short prev_power = -1;
static short prev_hour = -1;
static short prev_day = -1;
static char date[16];

static volatile bool zero_prefix    = false;
static volatile bool show_power     = true;
static volatile bool show_bluetooth = true;
static volatile bool month_first    = true;
static volatile bool vibe_hour      = true;
static volatile bool vibe_bluetooth = false;
static char day_text[7][4];

/**
 * Callback to notify when Application Sync Error occurred
 */
static void sync_error_callback (DictionaryResult dict_error,AppMessageResult app_message_error,void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR,"App Message Sync Error: %d",app_message_error);
} //sync_error_callback

/**
 * Loads the digit image from the application's resources and
 * displays it on-screen in the correct location.
 *
 * Each slot is a quarter of the screen
 */
static void load_digit_image_into_slot (int slot_number,int digit_value) {
  // TODO: Signal these error's
  if((slot_number >= 0) && (slot_number < TOTAL_IMAGE_SLOTS)) {
    if((digit_value >= 0) && (digit_value <= 9)) {
      if(image_slot_state[slot_number] == EMPTY_SLOT) {
        images[slot_number] = gbitmap_create_with_resource(IMAGE_RESOURCE_IDS[digit_value]);
        GRect frame = (GRect) {
          .origin = { (slot_number % 2) * 72,(slot_number / 2) * 74 },
          .size = images[slot_number]->bounds.size
        };
        image_layers[slot_number] = bitmap_layer_create(frame);
        bitmap_layer_set_bitmap(image_layers[slot_number],images[slot_number]);
        layer_add_child(window_get_root_layer(window),bitmap_layer_get_layer(image_layers[slot_number]));
        image_slot_state[slot_number] = digit_value;
      }
    }
  }
} //load_digit_image_into_slot

/**
 * Removes the digit from the display and unloads the image resource
 * to free up RAM.
 *
 * Can handle being called on an already empty slot.
 */
static void unload_digit_image_from_slot (int slot_number) {
  if(image_slot_state[slot_number] != EMPTY_SLOT) {
    layer_remove_from_parent(bitmap_layer_get_layer(image_layers[slot_number]));
    bitmap_layer_destroy(image_layers[slot_number]);
    gbitmap_destroy(images[slot_number]);
    image_slot_state[slot_number] = EMPTY_SLOT;
  }
} //unload_digit_image_from_slot

/**
 * Displays a numeric value between 0 and 99 on screen.
 *
 * Rows are ordered on screen as:
 *   Row 0
 *   Row 1
 *
 * Includes optional blanking of first leading zero,
 *   i.e. displays ' 0' rather than '00'.
 */
static void display_value (unsigned short value,unsigned short row_number,bool changed) {
  value %= 100; // Maximum of two digits per row.
    // Column order is: | Column 0 | Column 1 |
    // (We process the columns in reverse order because that makes
    //  extracting the digits from the value easier.)
  for(int col_number = 1;col_number >= 0;col_number--) {
    int slot_number = (row_number * 2) + col_number;
    short digit = value % 10;
    if(changed || (digit != image_slot_state[slot_number])) {
      unload_digit_image_from_slot(slot_number);
      if(zero_prefix || (digit != 0) || (slot_number != 0)) {
        load_digit_image_into_slot(slot_number,digit);
      }
    }
    value /= 10;
  }
} //display_value

static unsigned short get_display_hour (unsigned short hour) {
  if(clock_is_24h_style()) {
    return hour;
  } else {
    unsigned short display_hour = hour % 12;
      // Converts "0" to "12"
    return display_hour ? display_hour : 12;
  }
} //get_display_hour

static void display_time (struct tm *tick_time,bool changed) {
  display_value(get_display_hour(tick_time->tm_hour),0,changed);
  display_value(tick_time->tm_min,1,changed);
} //display_time

static void display_date (struct tm *tick_time) {
  int date1 = (month_first) ? tick_time->tm_mon + 1 : tick_time->tm_mday;
  int date2 = (month_first) ? tick_time->tm_mday : tick_time->tm_mon + 1;
  snprintf(date,16,"%s %d/%d",day_text[tick_time->tm_wday],date1,date2);
  text_layer_set_text(text_layer,date);
} //display_date

static void handle_minute_tick (struct tm *tick_time,TimeUnits units_changed) {
  display_time(tick_time,false);
  if(prev_day != tick_time->tm_wday) {
    display_date(tick_time);
    prev_day = tick_time->tm_wday;
  }
  if(vibe_hour && (prev_hour != tick_time->tm_hour)) {
    vibes_double_pulse();
    prev_hour = tick_time->tm_hour;
  }
} //handle_minute_tick

static void update_time () {
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  display_time(tick_time,true);
} //update_time

static void update_date () {
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  display_date(tick_time);
} //update_date

static void handle_power_level (BatteryChargeState charge_state) {
  if(show_power) {
    short power_level = -1;
    if(charge_state.is_charging) {
      power_level = 5;  
    } else {
      power_level = (charge_state.charge_percent - 1) / 20;
    }
    if(power_level != prev_power) {
        // Load and Display the Power Level Indicator
      power_image = gbitmap_create_with_resource(POWER_IMAGE_RESOURCE_IDS[power_level]);
      GRect frame = (GRect) {
//        .origin = { 31,150 }, <-- Centered under tens digits
        .origin = { 5,150 },    //  Left aligned (5px border)
        .size = power_image->bounds.size
      };
      if(power_layer == NULL) {
        power_layer = bitmap_layer_create(frame);
      }
      bitmap_layer_set_bitmap(power_layer,power_image);
      layer_add_child(window_get_root_layer(window),bitmap_layer_get_layer(power_layer));
      prev_power = power_level;
    }
  } else {
    if(power_image != NULL) {
      layer_remove_from_parent(bitmap_layer_get_layer(power_layer));
      bitmap_layer_destroy(power_layer);
      power_layer = NULL;
      gbitmap_destroy(power_image);
      power_image = NULL;
      prev_power = -1;
    }
  }
} //handle_power_level

static void hide_bluetooth () {
  layer_remove_from_parent(bitmap_layer_get_layer(bluetooth_layer));
  bitmap_layer_destroy(bluetooth_layer);
  bluetooth_layer = NULL;
  gbitmap_destroy(bluetooth_image);
  bluetooth_image = NULL;
} //hide_bluetooth

static void handle_connection (bool connected) {
  if(connected != prev_bluetooth) {
    if(show_bluetooth) {
      if(connected) {
          //Display the Bluetooth Image Layer
        if(bluetooth_image == NULL) {
          bluetooth_image = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_BLUETOOTH);
          GRect frame = (GRect) {
//            .origin = { 103,150 }, <-- Centered under ones digits
            .origin = { 129,150 },   //  Right aligned (5px border)
            .size = bluetooth_image->bounds.size
          };
          if(bluetooth_layer == NULL) {
            bluetooth_layer = bitmap_layer_create(frame);
          }
          bitmap_layer_set_bitmap(bluetooth_layer,bluetooth_image);
          layer_add_child(window_get_root_layer(window),bitmap_layer_get_layer(bluetooth_layer));
        }
      } else {
          // Hide the Bluetooth Image Layer
        if(bluetooth_image != NULL) {
          hide_bluetooth();
        }
      }
    } else {
        // Hide the Bluetooth Image Layer
      if(bluetooth_image != NULL) {
        hide_bluetooth();
        prev_bluetooth = false;
      }
    }    
    if(vibe_bluetooth) {
      vibes_enqueue_custom_pattern((connected) ? asc : desc);
    }
    prev_bluetooth = connected;
  }  
} //handle_connection

static bool get_tuple_bool_value (const Tuple * tuple) {
  switch(tuple->type) {
    case TUPLE_CSTRING:
      return tuple->length == 5;
    case TUPLE_INT:
      return tuple->value->int32 != 0;
    case TUPLE_UINT:
      return tuple->value->uint32 != 0;
    default:
      return false;
  }
} //get_tuple_bool_value

static void sync_day_text (const Tuple *tuple,int key,int day) {
  strcpy(day_text[day],tuple->value->cstring);
  if(prev_day == day) {
    update_date();
  }
  persist_write_string(key,day_text[day]);
} //sync_day_text

/**
 * Callback to notify when Application Settings change
 */
static void sync_tuple_changed_callback (const uint32_t key,const Tuple *new_tuple,const Tuple *old_tuple,void *context) {
//  APP_LOG(APP_LOG_LEVEL_DEBUG,"Tuple Key: %ld, Type: %d, Length: %d",new_tuple->key,new_tuple->type,new_tuple->length);
  switch(key) {
    case ZERO_PREFIX:
      zero_prefix = get_tuple_bool_value(new_tuple);
      update_time();
      persist_write_bool(ZERO_PREFIX,zero_prefix);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Zero Prefix Setting to watch = %s",zero_prefix ? "true" : "false");
      break;
    case SHOW_POWER:
      show_power = get_tuple_bool_value(new_tuple);
      handle_power_level(battery_state_service_peek());
      persist_write_bool(SHOW_POWER,show_power);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Power Indicator Setting to watch = %s",show_power ? "true" : "false");
      break;
    case SHOW_BTOOTH:
      show_bluetooth = get_tuple_bool_value(new_tuple);
      handle_connection(bluetooth_connection_service_peek());
      persist_write_bool(SHOW_POWER,show_bluetooth);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Bluetooth Indicator Setting to watch = %s",show_bluetooth ? "true" : "false");
      break;
    case MONTH_FIRST:
      month_first = get_tuple_bool_value(new_tuple);
      update_date();
      persist_write_bool(MONTH_FIRST,month_first);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Month First Indicator to watch = %s",month_first ? "month first" : "day first");
      break;
    case VIBE_HOUR:
      vibe_hour = get_tuple_bool_value(new_tuple);
      persist_write_bool(VIBE_HOUR,vibe_hour);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Vibe on Hour to watch = %s",vibe_hour ? "vibe" : "no vibe");
      break;
    case VIBE_BTOOTH:
      vibe_bluetooth = get_tuple_bool_value(new_tuple);
      persist_write_bool(VIBE_BTOOTH,vibe_bluetooth);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Vibe on Bluetooth Connection to watch = %s",vibe_bluetooth ? "vibe" : "no vibe");
      break;
    case SUN_TEXT:
      sync_day_text(new_tuple,SUN_TEXT,0);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Sunday Text to watch = %s",day_text[0]);
      break;
    case MON_TEXT:
      sync_day_text(new_tuple,MON_TEXT,1);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Monday Text to watch = %s",day_text[1]);
      break;
    case TUE_TEXT:
      sync_day_text(new_tuple,TUE_TEXT,2);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Tuesday Text to watch = %s",day_text[2]);
      break;
    case WED_TEXT:
      sync_day_text(new_tuple,WED_TEXT,3);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Wednesday Text to watch = %s",day_text[3]);
      break;
    case THU_TEXT:
      sync_day_text(new_tuple,THU_TEXT,4);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Thursday Text to watch = %s",day_text[4]);
      break;
    case FRI_TEXT:
      sync_day_text(new_tuple,FRI_TEXT,5);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Friday Text to watch = %s",day_text[5]);
      break;
    case SAT_TEXT:
      sync_day_text(new_tuple,SAT_TEXT,6);
//      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Saturday Text to watch = %s",day_text[6]);
      break;
  }
} //sync_tuple_changed_callback

static void send_cmd (void) {
  Tuplet value = TupletInteger(1,1);
  DictionaryIterator *i;
  app_message_outbox_begin(&i);
  if(i != NULL) {
    dict_write_tuplet(i,&value);
    dict_write_end(i);
    app_message_outbox_send();
  }
} //send_cmd

static char *persist_get_string (const uint32_t key,char *buffer,char *def) {
  if(persist_exists(key)) {
    persist_read_string(key,buffer,persist_get_size(key));
  } else {
    strcpy(buffer,def);
  }
  return buffer;
} //persist_get_string

static void app_init () {
    // Initialize Base Window
  window = window_create();
  window_stack_push(window,true);
    // Avoids a blank screen on watch start
  window_set_background_color(window,GColorBlack);
    // Retrieve Settings
  zero_prefix    = persist_exists(ZERO_PREFIX) ? persist_read_bool(ZERO_PREFIX) : false;
  show_power     = persist_exists(SHOW_POWER)  ? persist_read_bool(SHOW_POWER)  : true;
  show_bluetooth = persist_exists(SHOW_BTOOTH) ? persist_read_bool(SHOW_BTOOTH) : true;
  month_first    = persist_exists(MONTH_FIRST) ? persist_read_bool(MONTH_FIRST) : true;
  vibe_hour      = persist_exists(VIBE_HOUR)   ? persist_read_bool(VIBE_HOUR)   : true;
  vibe_bluetooth = persist_exists(VIBE_BTOOTH) ? persist_read_bool(VIBE_BTOOTH) : false;
  persist_get_string(SUN_TEXT,day_text[0],"Su");
  persist_get_string(MON_TEXT,day_text[1],"Mo");
  persist_get_string(TUE_TEXT,day_text[2],"Tu");
  persist_get_string(WED_TEXT,day_text[3],"We");
  persist_get_string(THU_TEXT,day_text[4],"Th");
  persist_get_string(FRI_TEXT,day_text[5],"Fr");
  persist_get_string(SAT_TEXT,day_text[6],"Sa");
    // Initialize Time Tick Handler
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  prev_hour = tick_time->tm_hour;
  display_time(tick_time,true);
  handle_power_level(battery_state_service_peek());
  handle_connection(bluetooth_connection_service_peek());
  tick_timer_service_subscribe(MINUTE_UNIT,handle_minute_tick);
  battery_state_service_subscribe(handle_power_level);
  bluetooth_connection_service_subscribe(handle_connection);

    // Date Text Layer
  GRect rect = (GRect) {
    .origin = { 17,148 },
      .size = { 110,20 }
  };
  text_layer = text_layer_create(rect);
  text_layer_set_text_color(text_layer,GColorWhite);
  text_layer_set_background_color(text_layer,GColorBlack);
  text_layer_set_text_alignment(text_layer,GTextAlignmentCenter);
  text_layer_set_font(text_layer,fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  display_date(tick_time);
  prev_day = tick_time->tm_wday;
  layer_add_child(window_get_root_layer(window),text_layer_get_layer(text_layer));

    // Initialize the Sync Handler
  Tuplet initial_values[] = {
    TupletInteger(ZERO_PREFIX,false),
    TupletInteger(SHOW_POWER,true),
    TupletInteger(SHOW_BTOOTH,true),
    TupletInteger(MONTH_FIRST,true),
    TupletCString(SUN_TEXT,day_text[0]),
    TupletCString(MON_TEXT,day_text[1]),
    TupletCString(TUE_TEXT,day_text[2]),
    TupletCString(WED_TEXT,day_text[3]),
    TupletCString(THU_TEXT,day_text[4]),
    TupletCString(FRI_TEXT,day_text[5]),
    TupletCString(SAT_TEXT,day_text[6]),
    TupletInteger(VIBE_HOUR,true),
    TupletInteger(VIBE_BTOOTH,false)
  };
  app_sync_init(&sync,sync_buffer,sizeof(sync_buffer),initial_values,ARRAY_LENGTH(initial_values),sync_tuple_changed_callback,sync_error_callback,NULL);
  send_cmd();
  app_message_open(app_message_inbox_size_maximum(),app_message_outbox_size_maximum());
} //app_init

static void app_destroy () {
  tick_timer_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
  battery_state_service_unsubscribe();
  for(int i = 0;i < TOTAL_IMAGE_SLOTS;i++) {
    unload_digit_image_from_slot(i);
  }
  text_layer_destroy(text_layer);
  window_destroy(window);
  app_sync_deinit(&sync);
} //app_destroy

int main (void) {
  app_init();
  app_event_loop();
  app_destroy();
} //main
