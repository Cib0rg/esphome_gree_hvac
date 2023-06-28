#include <cmath>
#include "gree.h"
#include "esphome/core/macros.h"

namespace esphome {
namespace gree {

static const char *const TAG = "gree";

// block of byte positions in requests/answers
static const uint8_t FORCE_UPDATE = 7;
static const uint8_t MODE = 8;
static const uint8_t MODE_MASK = 0b11110000;
static const uint8_t FAN_MASK = 0b00001111;
static const uint8_t SWING = 12;
// change later to data_read/write sizeof ?
static const uint8_t CRC_WRITE = 46;
static const uint8_t CRC_READ = 49;
static const uint8_t TEMPERATURE = 9;
static const uint8_t INDOOR_TEMPERATURE = 46;

// component settings
static const uint8_t MIN_VALID_TEMPERATURE = 16;
static const uint8_t MAX_VALID_TEMPERATURE = 30;
static const uint8_t TEMPERATURE_STEP = 1;

// prints user configuration
void GreeClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Gree:");
  ESP_LOGCONFIG(TAG, "  Update interval: %u", this->get_update_interval());
  this->dump_traits_(TAG);
  this->check_uart_settings(4800, 1, uart::UART_CONFIG_PARITY_EVEN, 8);
  
}

void GreeClimate::loop() {
  if (this->available() >= sizeof(this->data_read_)) {
    this->read_array(this->data_read_, sizeof(this->data_read_));
    dump_message_("Read array", this->data_read_, sizeof(this->data_read_));
    // ignore packets with incorrect start bytes
    if (this->data_read_[0] != 126 || this->data_read_[1] != 126) {
      return;
    }
    // temporary ignore strange packets with 0x33 at [3]
    if (this->data_read_[3] == 51)
      return;
    read_state_(this->data_read_, sizeof(this->data_read_));
  }
}

/*
void GreeClimate::setup() {
  this->set_update_interval(300);
}
*/

void GreeClimate::update() {
  data_write_[CRC_WRITE] = get_checksum_(data_write_, sizeof(data_write_));
  send_data_(data_write_, sizeof(data_write_));
}

climate::ClimateTraits GreeClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.set_visual_min_temperature(MIN_VALID_TEMPERATURE);
  traits.set_visual_max_temperature(MAX_VALID_TEMPERATURE);
  traits.set_visual_temperature_step(TEMPERATURE_STEP);

  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_AUTO,
    climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_DRY,
    climate::CLIMATE_MODE_FAN_ONLY,
    climate::CLIMATE_MODE_HEAT
  });

  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH
  });

  // traits.set_supported_swing_modes(this->supported_swing_modes_);
  traits.set_supports_current_temperature(true);
  traits.set_supports_two_point_target_temperature(false);

  // traits.add_supported_preset(climate::CLIMATE_PRESET_NONE);
  // traits.add_supported_preset(climate::CLIMATE_PRESET_COMFORT);

  return traits;
}

void GreeClimate::read_state_(const uint8_t *data, uint8_t size) {

  uint8_t check = data[CRC_READ];
  uint8_t crc = get_checksum_(data, size);
  if (check != crc) {
    ESP_LOGW(TAG, "Invalid checksum");
    return;
  }

  this->target_temperature = data[TEMPERATURE] / 16 + MIN_VALID_TEMPERATURE;
  this->current_temperature = data[INDOOR_TEMPERATURE] - 40; // check later?

  // partially saving current state to control string
  data_write_[MODE] = data[MODE];
  // add target temperature state too? ok
  data_write_[TEMPERATURE] = data[TEMPERATURE];

  // update CLIMATE state according AC response
  switch (data[MODE] & MODE_MASK) {
    case AC_MODE_OFF:
      this->mode = climate::CLIMATE_MODE_OFF;
      break;
    case AC_MODE_AUTO:
      this->mode = climate::CLIMATE_MODE_AUTO;
      break;
    case AC_MODE_COOL:
      this->mode = climate::CLIMATE_MODE_COOL;
      break;
    case AC_MODE_DRY:
      this->mode = climate::CLIMATE_MODE_DRY;
      break;
    case AC_MODE_FANONLY:
      this->mode = climate::CLIMATE_MODE_FAN_ONLY;
      break;
    case AC_MODE_HEAT:
      this->mode = climate::CLIMATE_MODE_HEAT;
      break;
    default:
      ESP_LOGW(TAG, "Unknown AC MODE&fan: %s", data[MODE]);
  }

  // get current AC FAN SPEED from its response
  switch (data[MODE] & FAN_MASK) {
    case AC_FAN_AUTO:
      this->fan_mode = climate::CLIMATE_FAN_AUTO;
      break;
    case AC_FAN_LOW:
      this->fan_mode = climate::CLIMATE_FAN_LOW;
      break;
    case AC_FAN_MEDIUM:
      this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
      break;
    case AC_FAN_HIGH:
      this->fan_mode = climate::CLIMATE_FAN_HIGH;
      break;
    default:
      ESP_LOGW(TAG, "Unknown AC modeE&FAN: %s", data[MODE]);
  }

  /*
  switch (data[SWING]) {
    case AC_SWING_OFF:
      this->swing_mode = climate::CLIMATE_SWING_OFF;
      break;

    case AC_SWING_VERTICAL:
      this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
      break;

    case AC_SWING_HORIZONTAL:
      this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
      break;

    case AC_SWING_BOTH:
      this->swing_mode = climate::CLIMATE_SWING_BOTH;
      break;
  }
  */

  /*
  if (data[POWER] & COMFORT_PRESET_MASK) {
    this->preset = climate::CLIMATE_PRESET_COMFORT;
  } else {
    this->preset = climate::CLIMATE_PRESET_NONE;
  }
  */

  this->publish_state();
}

void GreeClimate::control(const climate::ClimateCall &call) {
  data_write_[FORCE_UPDATE] = 175;
  
/*
  // logging of saved mode&fan vars
  char str[250] = {0};
  char *pstr = str;
  for (int i = 0; i < sizeof(data_save_); i++) {
    pstr += sprintf(pstr, "%02X ", data_save_[i]);
  }
  ESP_LOGV(TAG, "SAVED: %s", str);
*/

  // saving mode&fan values from previous 
  uint8_t new_mode = data_write_[MODE] & MODE_MASK;
  uint8_t new_fan_speed = data_write_[MODE] & FAN_MASK;

  if (call.get_mode().has_value()) {
    switch (call.get_mode().value()) {
      case climate::CLIMATE_MODE_OFF:
        new_mode = AC_MODE_OFF;
        break;
      case climate::CLIMATE_MODE_AUTO:
        new_mode = AC_MODE_AUTO;
        break;
      case climate::CLIMATE_MODE_COOL:
        new_mode = AC_MODE_COOL;
        break;
      case climate::CLIMATE_MODE_DRY:
        new_mode = AC_MODE_DRY;
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        new_mode = AC_MODE_FANONLY;
        break;
      case climate::CLIMATE_MODE_HEAT:
        new_mode = AC_MODE_HEAT;
        break;
      default: // add warning to log?
        break;
    }
  }

  // set fan speed only if MODE != DRY (only LOW available)
  if (call.get_fan_mode().has_value() && new_mode != AC_MODE_DRY) {
    switch (call.get_fan_mode().value()) {
      case climate::CLIMATE_FAN_AUTO:
        new_fan_speed = AC_FAN_AUTO;
        break;
      case climate::CLIMATE_FAN_LOW:
        new_fan_speed = AC_FAN_LOW;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        new_fan_speed = AC_FAN_MEDIUM;
        break;
      case climate::CLIMATE_FAN_HIGH:
        new_fan_speed = AC_FAN_HIGH;
        break;
      default: // add warning to log?
        break;
    }
  }
  
  // set low speed when DRY mode because other speeds are not available
  if (new_mode == AC_MODE_DRY && new_fan_speed != AC_FAN_LOW) {
    new_fan_speed = AC_FAN_LOW;
  }

  /*
  if (call.get_preset().has_value()) {
    if (call.get_preset().value() == climate::CLIMATE_PRESET_COMFORT) {
      data_[POWER] |= COMFORT_PRESET_MASK;
    } else {
      data_[POWER] &= ~COMFORT_PRESET_MASK;
    }
  }
  */

  if (call.get_target_temperature().has_value()) {
    // check if temperature set in valid limits
    if (call.get_target_temperature().value() >= MIN_VALID_TEMPERATURE && call.get_target_temperature().value() <= MAX_VALID_TEMPERATURE)
      data_write_[TEMPERATURE] = (call.get_target_temperature().value() - MIN_VALID_TEMPERATURE) * 16;
  }

  // temporary disabled
  if (call.get_swing_mode().has_value()) {
    switch (call.get_swing_mode().value()) {
      case climate::CLIMATE_SWING_OFF:
        // data_[SWING] = SWING_OFF;
        break;
      case climate::CLIMATE_SWING_VERTICAL:
        // data_[SWING] = SWING_VERTICAL;
        break;
      case climate::CLIMATE_SWING_HORIZONTAL:
        // data_[SWING] = SWING_HORIZONTAL;
        break;
      case climate::CLIMATE_SWING_BOTH:
        // data_[SWING] = SWING_BOTH;
        break;
    }
  }

  data_write_[MODE] = new_mode + new_fan_speed;

  // compute checksum & send data
  data_write_[CRC_WRITE] = get_checksum_(data_write_, sizeof(data_write_));
  send_data_(data_write_, sizeof(data_write_));

  // change of force_update byte to "passive" state
  data_write_[FORCE_UPDATE] = 0;
}

void GreeClimate::send_data_(const uint8_t *message, uint8_t size) {
  this->write_array(message, size);
  dump_message_("Sent message", message, size);
}

void GreeClimate::dump_message_(const char *title, const uint8_t *message, uint8_t size) {
  ESP_LOGV(TAG, "%s:", title);
  char str[250] = {0};
  char *pstr = str;
  if (size * 2 > sizeof(str)) ESP_LOGE(TAG, "too long byte data");
  for (int i = 0; i < size; i++) {
    pstr += sprintf(pstr, "%02X ", message[i]);
  }
  ESP_LOGV(TAG, "%s", str);
}

uint8_t GreeClimate::get_checksum_(const uint8_t *message, size_t size) {
  // position of crc in packet
  uint8_t position = size - 1;
  uint8_t sum = 0;
  // ignore first 2 bytes & last one
  for (int i = 2; i < position; i++)
    sum += message[i];
  uint8_t crc = sum % 256;
  return crc;
}

}  // namespace gree
}  // namespace esphome
