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
  ZERO_PREFIX = 0x00, // boolean (1 byte = 1)
  SHOW_POWER  = 0x01, // boolean (1 byte = 2)
  SHOW_BTOOTH = 0x02, // boolean (1 byte = 3)
//  MONTH_FIRST = 0x03, // boolean (1 byte = 4)
//  SUN_TEXT = 0x04,    // string (4 bytes = 8)
//  MON_TEXT = 0x05,    // string (4 bytes = 12)
//  TUE_TEXT = 0x06,    // string (4 bytes = 16)
//  WED_TEXT = 0x07,    // string (4 bytes = 20)
//  THU_TEXT = 0x08,    // string (4 bytes = 24)
//  FRI_TEXT = 0x09,    // string (4 bytes = 28)
//  SAT_TEXT = 0x0A     // string (4 bytes = 32)
};

static AppSync sync;
static uint8_t sync_buffer[64];

static GBitmap *images[TOTAL_IMAGE_SLOTS];
static BitmapLayer *image_layers[TOTAL_IMAGE_SLOTS];
static GBitmap *bluetooth_image = NULL;
static BitmapLayer *bluetooth_layer;
static GBitmap *power_image = NULL;
static BitmapLayer *power_layer;

#define EMPTY_SLOT -1

// The state is either "empty" or the digit of the image currently in
// the slot
static int image_slot_state[TOTAL_IMAGE_SLOTS] = {EMPTY_SLOT, EMPTY_SLOT, EMPTY_SLOT, EMPTY_SLOT};
static bool prev_bluetooth = false;
static short prev_power = -1;

static bool zero_prefix    = false;
static bool show_power     = true;
static bool show_bluetooth = true;

/**
 * Callback to notify when Application Settings change
 *
 */
static void sync_tuple_changed_callback (const uint32_t key,const Tuple *new_tuple,const Tuple *old_tuple,void *context) {
  switch(key) {
    case ZERO_PREFIX:
      zero_prefix = new_tuple->value->int8;
      time_t now = time(NULL);
      struct tm *tick_time = localtime(&now);
      display_time(tick_time);
      persist_write_bool(ZERO_PREFIX,zero_prefix);
      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Zero Prefix Setting to watch = %s",zero_prefix ? "true" : "false");
      break;
    case SHOW_POWER:
      show_power = new_tuple->value->int8;
      handle_power_level(battery_state_service_peek());
      persist_write_bool(SHOW_POWER,show_power);
      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Power Indicator Setting t watch = %s",show_power ? "true" : "false");
      break;
    case SHOW_BTOOTH:
      show_bluetooth = new_tuple->value->int8;
      handle_connection(bluetooth_connection_service_peek());
      persist_write_bool(SHOW_POWER,show_power);
      APP_LOG(APP_LOG_LEVEL_DEBUG,"Saved new Power Indicator Setting t watch = %s",show_power ? "true" : "false");
      break;
  }
} //sync_tuple_changed_callback

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
        image_slot_state[slot_number] = digit;
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
static void display_value (unsigned short value,unsigned short row_number) {
  value %= 100; // Maximum of two digits per row.
    // Column order is: | Column 0 | Column 1 |
    // (We process the columns in reverse order because that makes
    //  extracting the digits from the value easier.)
  for(int col_number = 1;col_number >= 0;col_number--) {
    int slot_number = (row_number * 2) + col_number;
    short digit = value % 10;
    if(digit != image_slot_state[slot_number]) {
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

static void display_time (struct tm *tick_time) {
  display_value(get_display_hour(tick_time->tm_hour),0);
  display_value(tick_time->tm_min,1);
} //display_time

static void handle_minute_tick (struct tm *tick_time,TimeUnits units_changed) {
  display_time(tick_time);
} //handle_minute_tick

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
        .origin = { 31,150 },
        .size = power_image->bounds.size
      };
      power_layer = bitmap_layer_create(frame);
      bitmap_layer_set_bitmap(power_layer,power_image);
      layer_add_child(window_get_root_layer(window),bitmap_layer_get_layer(power_layer));
      prev_power = power_level;
    }
  }
} //handle_power_level

static void handle_connection (bool connected) {
  if(show_bluetooth && (connected != prev_bluetooth)) {
    if(connected) {
        //Display the Bluetooth Image Layer
      if(bluetooth_image == NULL) {
        bluetooth_image = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_BLUETOOTH);
        GRect frame = (GRect) {
          .origin = { 103,150 },
          .size = bluetooth_image->bounds.size
        };
        bluetooth_layer = bitmap_layer_create(frame);
        bitmap_layer_set_bitmap(bluetooth_layer,bluetooth_image);
        layer_add_child(window_get_root_layer(window),bitmap_layer_get_layer(bluetooth_layer));
      }
    } else {
        // Hide the Bluetooth Image Layer
      if(bluetooth_image != NULL) {
        layer_remove_from_parent(bitmap_layer_get_layer(bluetooth_layer));
        bitmap_layer_destroy(bluetooth_layer);
        gbitmap_destroy(bluetooth_image);
        bluetooth_image = NULL;
      }
    }
    prev_bluetooth = connected;
  }  
} //handle_connection

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
    // Initialize Time Tick Handler
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  display_time(tick_time);
  handle_power_level(battery_state_service_peek());
  handle_connection(bluetooth_connection_service_peek());
  tick_timer_service_subscribe(MINUTE_UNIT,handle_minute_tick);
  battery_state_service_subscribe(handle_power_level);
  bluetooth_connection_service_subscribe(handle_connection);
    // Initialize the Sync Handler
  Tuplet initial_values[] = {
    TupletInteger(ZERO_PREFIX,false),
    TupletInteger(SHOW_POWER,true),
    TupletInteger(SHOW_BTOOTH,true)
  }
  app_sync_init(&sync,sync_buffer,sizeof(sync_buffer),initial_values,ARRAY_LENGTH(initial_values),sync_tuple_changed_callback,sync_error_callback,NULL);
} //app_init

static void app_destroy () {
  tick_timer_service_unsubscribe();
  bluetooth_connection_service_unsubscribe();
  battery_state_service_unsubscribe();
  for(int i = 0;i < TOTAL_IMAGE_SLOTS;i++) {
    unload_digit_image_from_slot(i);
  }
  window_destroy(window);
  app_sync_deinit(&sync);
} //app_destroy

int main (void) {
  app_init();
  app_event_loop();
  app_destroy();
} //main
