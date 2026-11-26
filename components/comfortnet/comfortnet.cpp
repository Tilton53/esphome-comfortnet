#include "comfortnet.h"
#include <array>
#include <utility>
#ifndef ARDUINO
#include "esp_timer.h"
#endif
#include "esphome/core/helpers.h"

namespace comfortnet {

static const char *const TAG = "comfortnet";

static const uint32_t RECEIVE_TIMEOUT = 500;   // How long we will wait to receive a full message
static const uint32_t REQUEST_TIMEOUT = 3000;  // How long we will wait for a reply to a request
static const uint32_t R2R_FALLBACK_TIMEOUT = 15000;  // Switch to direct-poll fallback if R2R is absent
static const uint32_t NETWORK_TIMEOUT =
    120000;  // We need to make sure our address is renewed at least every 120 seconds (Networking Specification 9.1.2)
static const uint32_t SILENCE_TIMEOUT = 60000;  // How long we will wait for any message, before assuming the network is
                                                // unavailable (This is not official spec,
                                                // but for monitoring purposes. May need adjustment.)

// Packet information
static const uint8_t PACKET_HEADER_SIZE = 10;
static const uint8_t PACKET_CRC_SIZE = 2;

// Header indices (Relative to packet)
static const uint8_t DESTINATION_ADDRESS_POS = 0;
static const uint8_t SOURCE_ADDRESS_POS = 1;
static const uint8_t SUBNET_POS = 2;
static const uint8_t SEND_METHOD_POS = 3;
static const uint8_t SEND_PARAMETER_1_POS = 4; /* Extra data for send method */
static const uint8_t SEND_PARAMETER_2_POS = 5; /* Generally 0 on sending, as coordinator will fill it with the ID of the
                                    target device when routing packets */
static const uint8_t SOURCE_NODE_TYPE_POS = 6;
static const uint8_t MESSAGE_TYPE_POS = 7;
static const uint8_t PACKET_NUMBER_POS =
    8; /* Defined in ClimateTalk Alliance CT2.0 CT-485 API Reference Revision 01 4.3 - Packet Number*/
static const uint8_t PAYLOAD_LENGTH_POS = 9; /* 0-MAX_PAYLOAD_SIZE */

static const uint8_t CMD_HEADER_SIZE = 2;

// Data indices (Relative to data, not packet)
static const uint8_t ACK_POS = 0;

static const uint8_t CONTROL_CMD_POS = 0;
static const uint8_t CONTROL_CMD_SIZE = 2;  // Length of control command header

static const uint8_t DISCOVERY_NODE_TYPE_POS = 0;

static const uint8_t TOKEN_OFFER_NODE_TYPE_POS = 0;

static const uint8_t ADDRESS_NODE_ID_POS = 0;
static const uint8_t ADDRESS_SUBNET_POS = 1;
static const uint8_t ADDRESS_MAC_POS = 2;

// ComfortnetData keys for listeners
static const std::string DATA_KEY_NETWORK_STATUS = "NETWORK_STATUS";
static const std::string DATA_KEY_PENDING_MESSAGE_COUNT = "PENDING_MESSAGE_COUNT";

void Comfortnet::queue_message(PendingMessage message) {
  pending_messages_.push(std::move(message));
  publish_pending_message_count_();
}

void Comfortnet::queue_next_user_menu_probe_() {
  static const std::array<std::array<uint8_t, 3>, 32> probes = []() {
    std::array<std::array<uint8_t, 3>, 32> result{};
    size_t index = 0;
    for (uint8_t menu = 0; menu < 4; menu++) {
      for (uint8_t item = 0; item < 4; item++) {
        // Only actions 0 and 1 are documented reads. Do not probe undocumented action values.
        for (uint8_t action = 0; action < 2; action++) {
          result[index++] = {menu, item, action};
        }
      }
    }
    return result;
  }();

  if (!user_menu_probe_started_ || user_menu_probe_index_ >= probes.size()) {
    return;
  }

  const auto &probe = probes[user_menu_probe_index_++];
  std::vector<uint8_t> request_payload{probe[0], probe[1], probe[2]};
  queue_message(PendingMessageToType(NodeType::GAS_FURNACE, MessageType::GET_USER_MENU,
                                     std::move(request_payload)));
  ESP_LOGI(TAG, "Queued user/installer menu probe %u/32 (menu=%u item=%u action=%u)",
           static_cast<unsigned int>(user_menu_probe_index_), probe[0], probe[1], probe[2]);
}

void Comfortnet::publish_pending_message_count_() {
  call_listener_(DATA_KEY_PENDING_MESSAGE_COUNT,
                 (struct ComfortnetData) {device_type_, ComfortnetData::DataType::FLOAT,
                                          static_cast<float>(pending_messages_.size())});
}

void Comfortnet::log_unknown_furnace_mdi_(MessageType message_type, const uint8_t *payload, uint8_t payload_len) {
  const MessageType request_type = PACKET_REQUEST(message_type);
  if (request_type != MessageType::GET_CONFIGURATION && request_type != MessageType::GET_STATUS &&
      request_type != MessageType::GET_SENSOR_DATA) {
    return;
  }

  uint16_t offset = 0;
  while (offset < payload_len) {
    const uint16_t remaining = payload_len - offset;
    if (remaining < 2) {
      char trailing_hex[3 * MAX_PAYLOAD_SIZE + 1];
      esphome::format_hex_pretty_to(trailing_hex, sizeof(trailing_hex), payload + offset, remaining, '.');
      ESP_LOGW(TAG, "Furnace MDI response 0x%02X has %u trailing byte(s) without a DBID/length: %s",
               static_cast<unsigned int>(message_type), static_cast<unsigned int>(remaining), trailing_hex);
      return;
    }

    const uint8_t dbid = payload[offset++];
    const uint8_t data_len = payload[offset++];
    const uint16_t available = payload_len - offset;
    const uint8_t *item_data = payload + offset;
    char value_hex[3 * MAX_PAYLOAD_SIZE + 1];
    const uint8_t value_len = static_cast<uint8_t>(std::min<uint16_t>(data_len, available));
    esphome::format_hex_pretty_to(value_hex, sizeof(value_hex), item_data, value_len, '.');

    if (data_len > available) {
      ESP_LOGW(TAG,
               "Malformed furnace MDI response 0x%02X: DBID=0x%02X declares %u value byte(s), only %u remain; value=%s",
               static_cast<unsigned int>(message_type), dbid, data_len, static_cast<unsigned int>(available), value_hex);
      return;
    }

    bool known_item = false;
    int expected_length = -1;
    bool known_value = true;
    if (request_type == MessageType::GET_CONFIGURATION) {
      switch (dbid) {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x06:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x18:
          known_item = true;
          expected_length = 1;
          break;
        case 0x07:
        case 0x08:
        case 0x14:
          known_item = true;
          expected_length = 2;
          break;
        default:
          break;
      }
      if (known_item && data_len == expected_length) {
        switch (dbid) {
          case 0x02:
            known_value = item_data[0] == 1 || item_data[0] == 2 || item_data[0] == 100;
            break;
          case 0x03:
            known_value = item_data[0] == 1 || item_data[0] == 2;
            break;
          case 0x04:
            known_value = item_data[0] <= 3;
            break;
          case 0x06:
            known_value = item_data[0] <= 2;
            break;
          case 0x09:
            known_value = item_data[0] <= 3;
            break;
          case 0x0A:
          case 0x0B: {
            const int8_t trim = static_cast<int8_t>(item_data[0]);
            known_value = trim >= -15 && trim <= 15 && trim % 5 == 0;
            break;
          }
          case 0x11:
            known_value = item_data[0] <= 1;
            break;
          case 0x12:
            known_value = item_data[0] <= 2;
            break;
          case 0x10:
            known_value = item_data[0] >= 25 && item_data[0] <= 100;
            break;
          default:
            break;
        }
      }
    } else {
      if (request_type == MessageType::GET_STATUS && dbid == 0x00) {
        // The standard furnace status block contains 22 bytes of positional status items.
        known_item = true;
        expected_length = 22;
      } else {
        switch (dbid) {
          case 0x00:  // Existing furnace sensor MDI uses zero-based return-air/supply-air IDs.
          case 0x01:
          case 0x02:
          case 0x03:
          case 0x04:
          case 0x05:
          case 0x0A:
          case 0x0B:
          case 0x0C:
          case 0x0D:
          case 0x0E:
          case 0x0F:
          case 0x10:
          case 0x15:
          case 0x16:
            known_item = true;
            expected_length = 2;
            break;
          case 0x12:
          case 0x20:
          case 0x21:
            known_item = true;
            expected_length = 1;
            break;
          default:
            break;
        }
        if (request_type == MessageType::GET_SENSOR_DATA && known_item && data_len == expected_length) {
          if (dbid == 0x20) {
            known_value = item_data[0] <= 0x03;
          } else if (dbid == 0x21) {
            known_value = (item_data[0] & 0xFC) == 0;
          }
        }
      }
    }

    if (!known_item) {
      ESP_LOGI(TAG, "Unknown furnace %s DBID=0x%02X len=%u value=%s",
               request_type == MessageType::GET_CONFIGURATION ? "configuration" :
               request_type == MessageType::GET_STATUS ? "status" : "sensor",
               dbid, data_len, value_hex);
    } else if (data_len != expected_length) {
      ESP_LOGI(TAG, "Unexpected furnace %s DBID=0x%02X length=%u (known length=%d) value=%s",
               request_type == MessageType::GET_CONFIGURATION ? "configuration" :
               request_type == MessageType::GET_STATUS ? "status" : "sensor",
               dbid, data_len, expected_length, value_hex);
    } else if (!known_value) {
      ESP_LOGI(TAG, "Unknown furnace configuration DBID=0x%02X value=%s", dbid, value_hex);
    }
    offset += data_len;
  }
}

bool Comfortnet::queue_control_command_(NodeType target_type, CommandType command,
                                        const std::vector<uint8_t> &command_data) {
  if (node_id_ == static_cast<NodeAddress>(0)) {
    ESP_LOGW(TAG, "Cannot send control command while not joined to the ClimateTalk network");
    return false;
  }
  if (command_data.size() + 2 > MAX_PAYLOAD_SIZE) {
    ESP_LOGW(TAG, "Control command payload is too large");
    return false;
  }

  std::vector<uint8_t> payload;
  payload.reserve(command_data.size() + 2);
  const uint16_t command_id = static_cast<uint16_t>(command);
  payload.push_back(static_cast<uint8_t>(command_id & 0xFF));
  payload.push_back(static_cast<uint8_t>((command_id >> 8) & 0xFF));
  payload.insert(payload.end(), command_data.begin(), command_data.end());
  queue_message(PendingMessageToType(target_type, MessageType::SET_CONTROL_COMMAND, std::move(payload)));
  const PendingMessage &queued = pending_messages_.back();
  char payload_hex[3 * MAX_PAYLOAD_SIZE + 1];
  esphome::format_hex_pretty_to(payload_hex, sizeof(payload_hex), queued.payload.data(), queued.payload.size(), '.');
  ESP_LOGI(TAG,
           "Queued command=0x%04X target_type=0x%02X msg=0x%02X method=0x%02X route=0x%02X queue_depth=%u payload=%s",
           command_id, static_cast<unsigned int>(target_type), static_cast<unsigned int>(queued.packet_type),
           static_cast<unsigned int>(queued.send_method), queued.send_param_1,
           static_cast<unsigned int>(pending_messages_.size()), payload_hex);
  return true;
}

bool Comfortnet::queue_next_poll_(uint32_t now, bool allow_queued_messages) {
  if (!polling_due_ || poll_request_pending_ || (!allow_queued_messages && !pending_messages_.empty()) ||
      polling_queue_.empty()) {
    return false;
  }

  const PollQueueEntry &dev = polling_queue_.front();
  if (dev.poll_message != MessageType::GET_STATUS && dev.poll_message != MessageType::GET_SENSOR_DATA &&
      dev.poll_message != MessageType::GET_IDENTIFICATION && dev.poll_message != MessageType::GET_CONFIGURATION) {
    ESP_LOGW(TAG, "Unable to handle poll request for message type 0x%02X to node type 0x%02X",
             static_cast<unsigned int>(dev.poll_message), static_cast<unsigned int>(dev.node_type));
    polling_queue_.erase(polling_queue_.begin());
    return false;
  }

  pending_messages_.push((struct PendingMessageToType) {dev.node_type, dev.poll_message, {}});
  polling_due_ = false;
  poll_request_pending_ = true;
  pending_poll_message_ = dev.poll_message;
  pending_poll_node_type_ = dev.node_type;
  last_poll_enqueue_time_ = now;
  publish_pending_message_count_();
  return true;
}

bool Comfortnet::thermostat_set_heat_setpoint(int32_t temperature_f) {
  if (temperature_f < 40 || temperature_f > 100) {
    ESP_LOGW(TAG, "Heat setpoint must be between 40 and 100 degrees Fahrenheit");
    return false;
  }
  return queue_control_command_(NodeType::THERMOSTAT, CommandType::HEAT_SET_POINT_TEMPERATURE_MODIFY,
                                {static_cast<uint8_t>(temperature_f)});
}

bool Comfortnet::thermostat_set_cool_setpoint(int32_t temperature_f) {
  if (temperature_f < 40 || temperature_f > 100) {
    ESP_LOGW(TAG, "Cool setpoint must be between 40 and 100 degrees Fahrenheit");
    return false;
  }
  return queue_control_command_(NodeType::THERMOSTAT, CommandType::COOL_SET_POINT_TEMPERATURE_MODIFY,
                                {static_cast<uint8_t>(temperature_f)});
}

bool Comfortnet::thermostat_set_system_mode(int32_t mode) {
  if (mode < 0 || mode > 4) {
    ESP_LOGW(TAG, "System mode must be 0=off, 1=cool, 2=auto, 3=heat, or 4=backup heat");
    return false;
  }
  return queue_control_command_(NodeType::THERMOSTAT, CommandType::SYSTEM_SWITCH_MODIFY,
                                {static_cast<uint8_t>(mode)});
}

bool Comfortnet::thermostat_set_fan_key(int32_t state, int32_t demand) {
  if (state < 0 || state > 1) {
    ESP_LOGW(TAG, "Fan key state must be 0=auto or 1=manual");
    return false;
  }
  std::vector<uint8_t> command_data{static_cast<uint8_t>(state)};
  if (state == 1) {
    if (demand < 0 || demand > 255) {
      ESP_LOGW(TAG, "Manual fan demand must fit in one byte (0-255)");
      return false;
    }
    command_data.push_back(static_cast<uint8_t>(demand));
  }
  return queue_control_command_(NodeType::THERMOSTAT, CommandType::FAN_KEY_SELECTION, command_data);
}

bool Comfortnet::thermostat_set_hold_override(bool enabled) {
  return queue_control_command_(NodeType::THERMOSTAT, CommandType::HOLD_OVERRIDE,
                                {static_cast<uint8_t>(enabled ? 1 : 0)});
}

bool Comfortnet::furnace_set_demand(CommandType command, int32_t demand_percent, int32_t duration_minutes) {
  if (command != CommandType::HEAT_DEMAND && command != CommandType::COOL_DEMAND &&
      command != CommandType::BACK_UP_HEAT_DEMAND && command != CommandType::DEFROST_HEAT_DEMAND &&
      command != CommandType::AUX_HEAT_DEMAND) {
    ESP_LOGW(TAG, "Unsupported furnace demand command 0x%04X", static_cast<unsigned int>(command));
    return false;
  }
  if (demand_percent < 0 || demand_percent > 100 || duration_minutes < 1 || duration_minutes > 15) {
    ESP_LOGW(TAG, "Furnace demand requires 0-100 percent and a 1-15 minute refresh duration");
    return false;
  }
  // The demand timer uses its high nibble for minutes; the demand byte is in half-percent units.
  return queue_control_command_(NodeType::GAS_FURNACE, command,
                                {static_cast<uint8_t>(duration_minutes << 4),
                                 static_cast<uint8_t>(demand_percent * 2)});
}

bool Comfortnet::furnace_set_fan_demand(int32_t mode, int32_t demand_percent, int32_t duration_minutes) {
  if (mode < 0 || mode > 5 || demand_percent < 0 || demand_percent > 100 || duration_minutes < 1 ||
      duration_minutes > 15) {
    ESP_LOGW(TAG, "Fan demand requires mode 0-5, demand 0-100 percent, and duration 1-15 minutes");
    return false;
  }
  return queue_control_command_(NodeType::GAS_FURNACE, CommandType::FAN_DEMAND,
                                {static_cast<uint8_t>(duration_minutes << 4), static_cast<uint8_t>(mode),
                                 static_cast<uint8_t>(demand_percent * 2)});
}

void Comfortnet::setup() {
  if (flow_control_pin_ != nullptr) {
    flow_control_pin_->setup();
  }
  mac_address_.setRandom();
}

void Comfortnet::reconnect() {
  ESP_LOGI(TAG, "Reconnect requested; resetting ClimateTalk network session");
  this->disconnect_();
  ESP_LOGI(TAG, "Waiting for a network discovery broadcast to rejoin");
}

void Comfortnet::dump_config() {
  ESP_LOGCONFIG(TAG, "ComfortNet:");
  LOG_PIN("  Flow Control Pin: ", this->flow_control_pin_);
  ESP_LOGCONFIG(TAG, "  MAC: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", mac_address_.mac[0], mac_address_.mac[1],
                mac_address_.mac[2], mac_address_.mac[3], mac_address_.mac[4], mac_address_.mac[5], mac_address_.mac[6],
                mac_address_.mac[7]);
  ESP_LOGCONFIG(TAG, "  Device Type: %02x", static_cast<unsigned int>(device_type_));
}

uint16_t Comfortnet::calculate_crc_(const uint8_t *data, uint8_t data_len) {
  uint8_t sum1 = 0xAA;  // Fletcher seed
  uint8_t sum2 = 0;

  for (short i = 0; i < data_len; i++) {
    sum1 = (sum1 + data[i]) % 0xFF;
    sum2 = (sum2 + sum1) % 0xFF;
  }

  uint8_t tmp = 0xFF - ((sum1 + sum2) % 0xFF);
  return (static_cast<uint16_t>(tmp) << 8) | static_cast<uint16_t>(0xFF - ((sum1 + tmp) % 0xFF));
}

/**
 * Defined in ClimateTalk Alliance CT2.0 CT-485 Networking Specification Revision 01
 * 11.1 Slot Delay
 */
#define MINIMUM_SLOT_DELAY \
  100  // Additionally, bus must be silent for at least 100ms before we can speak (Networking Specification 9.5)
#define MAXIMUM_SLOT_DELAY 2500

uint32_t Comfortnet::generate_slot_delay_() {
#ifdef ARDUINO
  return random(MINIMUM_SLOT_DELAY, MAXIMUM_SLOT_DELAY);
#else
  return (esp_random() % (MAXIMUM_SLOT_DELAY - MINIMUM_SLOT_DELAY + 1)) + MINIMUM_SLOT_DELAY;
#endif
}

void Comfortnet::set_node_list_(const uint8_t *data, uint8_t data_len) {
  for (uint8_t i = 0; i < data_len; i++) {
    node_list_[i] = static_cast<NodeType>(data[i]);
  }
  node_list_size_ = data_len;
}

void Comfortnet::handle_message_(bool is_tx, uint32_t now) {
  const uint8_t *data = is_tx ? tx_message_.data() : rx_message_.data();

  NodeAddress dst_adr = static_cast<NodeAddress>(data[DESTINATION_ADDRESS_POS]);
  NodeAddress src_adr = static_cast<NodeAddress>(data[SOURCE_ADDRESS_POS]);
  Subnet subnet = static_cast<Subnet>(data[SUBNET_POS]);
  SendMethod send_method = static_cast<SendMethod>(data[SEND_METHOD_POS]);
  uint8_t send_param_1 = data[SEND_PARAMETER_1_POS];
  uint8_t send_param_2 = data[SEND_PARAMETER_2_POS];
  NodeType source_node_type = static_cast<NodeType>(data[SOURCE_NODE_TYPE_POS]);
  MessageType message_type = static_cast<MessageType>(data[MESSAGE_TYPE_POS]);
  uint8_t packet_number = data[PACKET_NUMBER_POS];
  uint8_t payload_len = data[PAYLOAD_LENGTH_POS];
  // Packet data with len=payload_len
  const uint8_t *payload = data + PACKET_HEADER_SIZE;

  // Checksum validation by reading last 2 bytes after the data
  uint16_t crc = (data[PACKET_HEADER_SIZE + payload_len] << 8) | data[PACKET_HEADER_SIZE + payload_len + 1];
  uint16_t crc_check = calculate_crc_(data, PACKET_HEADER_SIZE + payload_len);
  if (crc != crc_check) {
    ESP_LOGW(TAG, "Checksum mismatch. Expected 0x%04X, got 0x%04X", crc, crc_check);
    return;
  }

  // The coordinator may keep an assigned node active through addressed traffic rather than
  // periodic ADDRESS_CONFIRMATION messages. Treat valid coordinator unicast traffic as
  // evidence that this local session is still recognized by the network.
  if (!is_tx && node_id_ != static_cast<NodeAddress>(0) && src_adr == NodeAddress::COORDINATOR &&
      dst_adr == node_id_ && subnet == subnet_) {
    this->last_address_confirm_time_ = now;
  }

  // Notice, the checksum and payload are printed out of order here for viewing convenience!
  ESP_LOGD(TAG,
           "Dir | Dest | Src  | Subnet | Meth | Params | SrcNode | MsgType | PktNum | Len | Checksum | Payload HEX");
  char payload_hex[3 * 255 + 1];
  esphome::format_hex_pretty_to(payload_hex, sizeof(payload_hex), payload, payload_len, '.');
  ESP_LOGD(
      TAG,
      "%s  | 0x%02X | 0x%02X | 0x%02X   | 0x%02X | 0x%04X | 0x%02X    | 0x%02X    | 0x%02X   | %-3u | 0x%04X   | %s",
      is_tx ? "TX" : "RX", static_cast<unsigned int>(dst_adr), static_cast<unsigned int>(src_adr),
      static_cast<unsigned int>(subnet), static_cast<unsigned int>(send_method), (send_param_1 << 8) | send_param_2,
      static_cast<unsigned int>(source_node_type),
      static_cast<unsigned int>(message_type), packet_number, payload_len, crc, payload_hex);
  if (is_tx) {
    if (this->direct_poll_command_waiting_tx_) {
      this->direct_poll_command_waiting_tx_ = false;
      this->direct_poll_command_waiting_response_ = true;
      this->direct_poll_command_tx_time_ = now;
    }
    if (message_type == MessageType::TOKEN_OFFER_RESPONSE) {
      // Most likely we won the token offer broadcast
      // We should only respond to 1 token offer per dataflow cycle to give other devices a chance
      has_won_token_broadcast_ = true;
    }
    // Stop here if this is a transmitted message
    return;
  }

  bool is_broadcast = dst_adr == NodeAddress::BROADCAST && (subnet == this->subnet_ || subnet == Subnet::BROADCAST);

  // Signals the start of a new dataflow cycle
  if (message_type == MessageType::NODE_DISCOVERY) {
    has_won_token_broadcast_ = false;
  }

  std::vector<uint8_t> return_payload;
  // Network member logic
  if (node_id_ != static_cast<NodeAddress>(0)) {
    /**
     * We are a network member
     */
    if (is_broadcast) {
      /**
       * Message is broadcast to us
       */
      if (message_type == MessageType::ADDRESS_CONFIRMATION) {
        uint8_t idnum = static_cast<uint8_t>(node_id_);
        this->last_address_confirm_time_ = now;
        if (idnum >= payload_len || static_cast<NodeType>(payload[idnum]) != device_type_) {
          ESP_LOGW(TAG, "Not in node list, disconnecting");
          disconnect_();
        }
      } else if (message_type == MessageType::SET_ADDRESS) {
        for (uint8_t i = 0; i < MAC_ADDRESS_SIZE; i++) {
          if (payload[ADDRESS_MAC_POS + i] != this->mac_address_.mac[i]) {
            return;  // Not our MAC
          }
        }
        for (uint8_t i = 0; i < SESSION_ID_SIZE; i++) {
          if (payload[ADDRESS_MAC_POS + MAC_ADDRESS_SIZE + i] != this->session_id_.sessionid[i]) {
            return;  // Not our session ID
          }
        }
        if (payload[ADDRESS_MAC_POS + MAC_ADDRESS_SIZE + SESSION_ID_SIZE] != 0x01) {
          ESP_LOGW(TAG, "Failed to get address!");  // Write byte must be 0x01
          return;
        }
        NodeAddress start_id = this->node_id_;
        this->last_address_confirm_time_ = now;
        this->node_id_ = static_cast<NodeAddress>(payload[ADDRESS_NODE_ID_POS]);
        this->subnet_ = static_cast<Subnet>(payload[ADDRESS_SUBNET_POS]);
        this->last_r2r_time_ = now;
        this->direct_poll_mode_ = false;
        this->direct_poll_command_waiting_tx_ = false;
        this->direct_poll_command_waiting_response_ = false;
        return_payload.push_back(static_cast<uint8_t>(this->node_id_));
        return_payload.push_back(static_cast<uint8_t>(this->subnet_));
        this->mac_address_.write(return_payload);
        this->session_id_.write(return_payload);
        return_payload.push_back(0x01);  // Write byte must be 0x01
        transmit_message_(NodeAddress::COORDINATOR, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0,
                          this->device_type_, PACKET_RESPONSE(message_type),
                          PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload);
        this->awaiting_discovery_ = false;
        ESP_LOGI(TAG, "Network address reassigned: 0x%02X (Old: 0x%02X)", static_cast<unsigned int>(this->node_id_),
                 static_cast<unsigned int>(start_id));
      } else if (message_type == MessageType::TOKEN_OFFER && !has_won_token_broadcast_ &&
                 (pending_messages_.size() > 0 || polling_queue_.size() > 0)) {
        NodeType offer_node_type = static_cast<NodeType>(payload[TOKEN_OFFER_NODE_TYPE_POS]);
        if (offer_node_type == NodeType::ANY || offer_node_type == this->device_type_) {
          return_payload.push_back(static_cast<uint8_t>(this->node_id_));
          return_payload.push_back(static_cast<uint8_t>(this->subnet_));
          this->mac_address_.write(return_payload);
          this->session_id_.write(return_payload);
          transmit_message_(NodeAddress::COORDINATOR, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0,
                            this->device_type_, PACKET_RESPONSE(message_type),
                            PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload, true);
        }
      }
    } else if (dst_adr == this->node_id_ && subnet == this->subnet_) {
      /**
       * Message is addressed to us
       */
      if (message_type == MessageType::SET_NETWORK_NODE_LIST) {
        /**
         * Core network packet
         */
        set_node_list_(payload, payload_len);
        if (this->direct_poll_mode_ && pending_messages_.size() > 0 &&
            !this->direct_poll_command_waiting_tx_ && !this->direct_poll_command_waiting_response_) {
          PendingMessage msg = pending_messages_.front();
          uint16_t command_id = 0;
          if (msg.packet_type == MessageType::SET_CONTROL_COMMAND && msg.payload.size() >= CMD_HEADER_SIZE) {
            command_id = static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8));
          }
          char payload_hex[3 * MAX_PAYLOAD_SIZE + 1];
          esphome::format_hex_pretty_to(payload_hex, sizeof(payload_hex), msg.payload.data(), msg.payload.size(), '.');
          ESP_LOGW(TAG,
                   "Direct-poll fallback: replacing 0x94 node-list response with queued msg=0x%02X command=0x%04X "
                   "method=0x%02X route=0x%02X queue_depth=%u payload=%s",
                   static_cast<unsigned int>(msg.packet_type), command_id, static_cast<unsigned int>(msg.send_method),
                   msg.send_param_1, static_cast<unsigned int>(pending_messages_.size()), payload_hex);
          return_payload.insert(return_payload.end(), msg.payload.begin(), msg.payload.end());
          this->direct_poll_command_waiting_tx_ = true;
          transmit_message_(src_adr, this->node_id_, this->subnet_, msg.send_method, msg.send_param_1, 0,
                            this->device_type_, msg.packet_type,
                            PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload);
        } else {
          transmit_message_(src_adr, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0, this->device_type_,
                            MessageType::SET_NETWORK_NODE_LIST_RESPONSE,
                            PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1),
                            reinterpret_cast<uint8_t *>(node_list_ + 0), node_list_size_);
        }
      } else if (message_type == MessageType::REQUEST_TO_RECEIVE_RESPONSE) {
        /**
         * R2R section
         */
        this->last_r2r_time_ = now;
        this->direct_poll_mode_ = false;
        this->direct_poll_command_waiting_response_ = false;
        queue_next_poll_(now);
        if (!this->r2r_reply_.empty()) {
          /**
           * We previously received a packet that this R2R is confirming
           */
          ESP_LOGI(TAG, "R2R slot: sending deferred response 0x%02X before pending queue (depth=%u)",
                   r2r_reply_.size() > MESSAGE_TYPE_POS ? r2r_reply_[MESSAGE_TYPE_POS] : 0,
                   static_cast<unsigned int>(pending_messages_.size()));
          tx_message_.clear();
          tx_message_.insert(tx_message_.end(), r2r_reply_.begin(), r2r_reply_.end());
          r2r_reply_.clear();
          message_queued_ = QueuedMessageType::NORMAL;
        } else if (pending_messages_.size() > 0) {
          /**
           * We have packets we need to send, send them!
           */
          PendingMessage msg = pending_messages_.front();
          uint16_t command_id = 0;
          if (msg.packet_type == MessageType::SET_CONTROL_COMMAND && msg.payload.size() >= CMD_HEADER_SIZE) {
            command_id = static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8));
          }
          char payload_hex[3 * MAX_PAYLOAD_SIZE + 1];
          esphome::format_hex_pretty_to(payload_hex, sizeof(payload_hex), msg.payload.data(), msg.payload.size(), '.');
          ESP_LOGI(TAG,
                   "R2R slot: sending queue front msg=0x%02X command=0x%04X method=0x%02X route=0x%02X queue_depth=%u payload=%s",
                   static_cast<unsigned int>(msg.packet_type), command_id, static_cast<unsigned int>(msg.send_method),
                   msg.send_param_1, static_cast<unsigned int>(pending_messages_.size()), payload_hex);
          return_payload.insert(return_payload.end(), msg.payload.begin(), msg.payload.end());
          transmit_message_(src_adr, this->node_id_, this->subnet_, msg.send_method, msg.send_param_1, 0,
                            this->device_type_, msg.packet_type,
                            PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload);
        } else {
          /**
           * We have nothing to send, just ACK
           */
          return_payload.push_back(R2R_ACK);
          this->mac_address_.write(return_payload);
          this->session_id_.write(return_payload);
          transmit_message_(src_adr, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0, this->device_type_,
                            MessageType::REQUEST_TO_RECEIVE_RESPONSE,
                            PACKET_NUMBER(true, this->subnet_ == Subnet::VERSION_1), return_payload);
        }
      } else {
        /**
         * All other packets addressed to us.
         * We should filter out packets we can't respond to and send NAKs or something instead of always sending an
         * ACK...
         */
        MessageAckAction should_ack = MessageAckAction::UNKNOWN;
        if (pending_messages_.size() > 0) {
          // Check if this is a reply to our request
          if (message_type == pending_messages_.front().packet_type &&
              send_param_1 == pending_messages_.front().send_param_1) {
            should_ack = MessageAckAction::NONE;
            if (payload_len < 1 || payload[ACK_POS] != R2R_ACK) {
              ESP_LOGW(TAG, "Corodinator did not ACK our 0x%02X", static_cast<unsigned int>(message_type));
            }
          } else if (message_type == PACKET_RESPONSE(pending_messages_.front().packet_type) &&
                     send_param_1 == pending_messages_.front().send_param_1) {
            should_ack = MessageAckAction::ACK;
            if (pending_messages_.front().packet_type == MessageType::GET_USER_MENU &&
                message_type == MessageType::GET_USER_MENU_RESPONSE) {
              char menu_payload_hex[3 * MAX_PAYLOAD_SIZE + 1];
              esphome::format_hex_pretty_to(menu_payload_hex, sizeof(menu_payload_hex), payload, payload_len, '.');
              const auto &request = pending_messages_.front().payload;
              if (request.size() == 3) {
                ESP_LOGI(TAG,
                         "User/installer menu response 0xC1 menu=%u item=%u action=%u len=%u payload=%s",
                         request[0], request[1], request[2], payload_len, menu_payload_hex);
              } else {
                ESP_LOGI(TAG, "User/installer menu response 0xC1 len=%u payload=%s", payload_len,
                         menu_payload_hex);
              }
            }
            if (poll_request_pending_ && pending_messages_.front().packet_type == pending_poll_message_ &&
                pending_messages_.front().send_method == SendMethod::NODE_TYPE &&
                pending_messages_.front().send_param_1 == static_cast<uint8_t>(pending_poll_node_type_)) {
              poll_request_pending_ = false;
            }
            ESP_LOGI(TAG, "Received response 0x%02X for queued request 0x%02X route=0x%02X; queue_depth=%u",
                     static_cast<unsigned int>(message_type),
                     static_cast<unsigned int>(pending_messages_.front().packet_type), send_param_1,
                     static_cast<unsigned int>(pending_messages_.size()));
            pending_messages_.pop();
            this->direct_poll_command_waiting_response_ = false;
            publish_pending_message_count_();
            if (message_type == MessageType::GET_USER_MENU_RESPONSE && this->user_menu_probe_started_) {
              queue_next_user_menu_probe_();
            }
          }
        }
        if (should_ack == MessageAckAction::UNKNOWN && message_type == MessageType::GET_NODE_ID) {
          should_ack = MessageAckAction::ACK;
          return_payload.push_back(static_cast<uint8_t>(this->device_type_));
          this->mac_address_.write(return_payload);
          this->session_id_.write(return_payload);
          write_message_to_buffer_(r2r_reply_, src_adr, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0,
                                   this->device_type_, PACKET_RESPONSE(message_type),
                                   PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload.data(),
                                   return_payload.size(), false, false);
          ESP_LOGD(TAG, "Deferred response 0x%02X queued for GET_NODE_ID; pending queue depth=%u",
                   static_cast<unsigned int>(PACKET_RESPONSE(message_type)),
                   static_cast<unsigned int>(pending_messages_.size()));
        } else if (should_ack == MessageAckAction::UNKNOWN && message_type == MessageType::GET_NODE_ID_RESPONSE) {
          should_ack = MessageAckAction::ACK;
        } else if (should_ack == MessageAckAction::UNKNOWN &&
                   message_type == MessageType::NETWORK_SHARED_DATA_SECTOR_IMAGE_READ_WRITE_REQUEST) {
          /**
           * For redundant data storage across all network nodes
           */
          NodeType requesting_type = static_cast<NodeType>(payload[0] & 0x7F);  // Clear bit 7 and extract the node type
          std::vector<uint8_t> *shared_data = nullptr;
          auto iter = network_shared_data_.find(requesting_type);
          if (iter == network_shared_data_.end()) {
            shared_data = &network_shared_data_[requesting_type];
          } else {
            shared_data = &iter->second;
          }
          should_ack = MessageAckAction::ACK;
          return_payload.push_back(static_cast<uint8_t>(requesting_type));
          if ((payload[0] & 0x80) == 0) {  // Write operation
            shared_data->clear();
            shared_data->insert(shared_data->end(), payload + 1, payload + payload_len);
          }
          return_payload.insert(return_payload.end(), shared_data->begin(), shared_data->end());
          write_message_to_buffer_(r2r_reply_, src_adr, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0,
                                   this->device_type_, PACKET_RESPONSE(message_type),
                                   PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload.data(),
                                   return_payload.size(), false, false);
        } else if (should_ack == MessageAckAction::UNKNOWN &&
                   message_type == MessageType::NETWORK_SHARED_DATA_SECTOR_IMAGE_READ_WRITE_REQUEST_RESPONSE) {
          should_ack = MessageAckAction::NONE;
          if (payload[ACK_POS] != R2R_ACK) {
            ESP_LOGW(TAG, "Received error during network shared data response!");
          }
        }
        if (should_ack == MessageAckAction::ACK) {
          return_payload.clear();
          return_payload.push_back(R2R_ACK);
          this->mac_address_.write(return_payload);
          this->session_id_.write(return_payload);
          transmit_message_(src_adr, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0, this->device_type_,
                            message_type, PACKET_NUMBER(true, this->subnet_ == Subnet::VERSION_1), return_payload);
        } else if (should_ack == MessageAckAction::NAK) {
          ESP_LOGW(TAG, "We are supposed to NAK to 0x%02X, but don't know how!", static_cast<unsigned int>(message_type));
        } else if (!PACKET_IS_DATAFLOW(packet_number) && should_ack == MessageAckAction::UNKNOWN) {
          ESP_LOGW(TAG, "We are supposed to respond to 0x%02X, but don't know how!",
                   static_cast<unsigned int>(message_type));
        }
      }
    }
  } else {
    /**
     * We are not yet a network member
     */
    if (!awaiting_discovery_ && message_type == MessageType::NODE_DISCOVERY) {
      NodeType discovery_node_type = static_cast<NodeType>(payload[DISCOVERY_NODE_TYPE_POS]);
      if (discovery_node_type == NodeType::ANY || discovery_node_type == this->device_type_) {
        ESP_LOGI(TAG, "Received discovery request, responding...");
        session_id_.setRandom();  // Generate a session ID
        return_payload.push_back(static_cast<uint8_t>(this->device_type_));
        return_payload.push_back(0x00);  // Reserved
        this->mac_address_.write(return_payload);
        this->session_id_.write(return_payload);
        transmit_message_(NodeAddress::COORDINATOR, this->node_id_, Subnet::BROADCAST, SendMethod::NO_ROUTE, 0, 0,
                          this->device_type_, PACKET_RESPONSE(message_type),
                          PACKET_NUMBER(false, this->ct_version_ == 1), return_payload, true);
        awaiting_discovery_ = true;
      }
    } else if (is_broadcast && message_type == MessageType::SET_ADDRESS) {
      for (uint8_t i = 0; i < MAC_ADDRESS_SIZE; i++) {
        if (payload[ADDRESS_MAC_POS + i] != this->mac_address_.mac[i]) {
          return;  // Not our MAC
        }
      }
      for (uint8_t i = 0; i < SESSION_ID_SIZE; i++) {
        if (payload[ADDRESS_MAC_POS + MAC_ADDRESS_SIZE + i] != this->session_id_.sessionid[i]) {
          return;  // Not our session ID
        }
      }
      if (payload[ADDRESS_MAC_POS + MAC_ADDRESS_SIZE + SESSION_ID_SIZE] != 0x01) {
        ESP_LOGW(TAG, "Failed to get address!");  // Write byte must be 0x01
        return;
      }
      NodeAddress start_id = this->node_id_;
      this->last_address_confirm_time_ = now;
      this->node_id_ = static_cast<NodeAddress>(payload[ADDRESS_NODE_ID_POS]);
      this->subnet_ = static_cast<Subnet>(payload[ADDRESS_SUBNET_POS]);
      this->last_r2r_time_ = now;
      this->direct_poll_mode_ = false;
      this->direct_poll_command_waiting_tx_ = false;
      this->direct_poll_command_waiting_response_ = false;
      return_payload.push_back(static_cast<uint8_t>(this->node_id_));
      return_payload.push_back(static_cast<uint8_t>(this->subnet_));
      this->mac_address_.write(return_payload);
      this->session_id_.write(return_payload);
      return_payload.push_back(0x01);  // Write byte must be 0x01
      transmit_message_(NodeAddress::COORDINATOR, this->node_id_, this->subnet_, SendMethod::NO_ROUTE, 0, 0,
                        this->device_type_, PACKET_RESPONSE(message_type),
                        PACKET_NUMBER(false, this->subnet_ == Subnet::VERSION_1), return_payload);
      this->awaiting_discovery_ = false;
      if (start_id == static_cast<NodeAddress>(0)) {
        ESP_LOGI(TAG, "Joined network as address: 0x%02X", static_cast<unsigned int>(this->node_id_));
        call_listener_(DATA_KEY_NETWORK_STATUS,
                       (struct ComfortnetData) {this->device_type_, ComfortnetData::DataType::BOOLEAN, true});
        if (!this->user_menu_probe_started_) {
          this->user_menu_probe_started_ = true;
          this->user_menu_probe_index_ = 0;
          queue_next_user_menu_probe_();
        }
      } else if (start_id != this->node_id_) {
        ESP_LOGI(TAG, "Network address reassigned: 0x%02X (Old: 0x%02X)", static_cast<unsigned int>(this->node_id_),
                 static_cast<unsigned int>(start_id));
      }
    }
  }
  // End network member logic

  // Network eavesdropping logic
  if (PACKET_IS_DATAFLOW(packet_number) && payload_len == 17 && payload[ACK_POS] == R2R_ACK &&
      src_adr != NodeAddress::BROADCAST &&
      static_cast<uint8_t>(src_adr) < MAX_PAYLOAD_SIZE) {  // Simple check for ACK messages
    for (uint8_t i = 0; i < MAC_ADDRESS_SIZE; i++) {
      this->node_mac_list_[static_cast<uint8_t>(src_adr)].mac[i] = payload[i + 1];  // Copy MAC to list
    }
  }
  if (message_type == MessageType::SET_CONTROL_COMMAND || message_type == MessageType::SET_CONTROL_COMMAND_RESPONSE) {
    if (message_type == MessageType::SET_CONTROL_COMMAND_RESPONSE && payload_len <= CONTROL_CMD_SIZE) {
      return;  // This is just an ACK, so we can't get any useful data from it
    }
    CommandType command_type = static_cast<CommandType>((payload[CONTROL_CMD_POS + 1] << 8) | payload[CONTROL_CMD_POS]);
    const uint8_t *cmd_payload = payload + CONTROL_CMD_SIZE;
    uint8_t cmd_payload_len = payload_len - CONTROL_CMD_SIZE;
    ESP_LOGD(TAG, "Command | Payload HEX");
    char cmd_payload_hex[3 * 255 + 1];
    esphome::format_hex_pretty_to(cmd_payload_hex, sizeof(cmd_payload_hex), cmd_payload, cmd_payload_len, '.');
    ESP_LOGD(TAG, "0x%04X  | %s", static_cast<unsigned int>(command_type), cmd_payload_hex);
    call_command_listener_((struct ComfortnetCommandData) {
        message_type == MessageType::SET_CONTROL_COMMAND ? get_node_type_(dst_adr) : source_node_type,
        get_node_mac_(message_type == MessageType::SET_CONTROL_COMMAND ? dst_adr : src_adr), command_type,
        message_type == MessageType::SET_CONTROL_COMMAND_RESPONSE, cmd_payload, cmd_payload_len});
  } else if (!PACKET_IS_DATAFLOW(packet_number) && (message_type == MessageType::GET_STATUS_RESPONSE ||
                                                    message_type == MessageType::GET_SENSOR_DATA_RESPONSE ||
                                                    message_type == MessageType::GET_CONFIGURATION_RESPONSE ||
                                                    message_type == MessageType::GET_IDENTIFICATION_RESPONSE)) {
    if (source_node_type == NodeType::GAS_FURNACE) {
      log_unknown_furnace_mdi_(message_type, payload, payload_len);
    }
    call_packet_listener_(
        (struct ComfortnetPacketData) {source_node_type, get_node_mac_(src_adr), message_type, payload, payload_len});
  }
  // End network eavesdropping logic
}

void Comfortnet::write_message_to_buffer_(std::vector<uint8_t> &buffer, NodeAddress dst_adr, NodeAddress src_adr,
                                          Subnet subnet, SendMethod send_method, uint8_t send_param_1,
                                          uint8_t send_param_2, NodeType src_node_type, MessageType msg_type,
                                          uint8_t packet_num, const uint8_t *data, uint8_t data_len, bool queue_send,
                                          bool require_arbitration) {
  buffer.clear();
  // Write packet header
  buffer.push_back(static_cast<uint8_t>(dst_adr));
  buffer.push_back(static_cast<uint8_t>(src_adr));
  buffer.push_back(static_cast<uint8_t>(subnet));
  buffer.push_back(static_cast<uint8_t>(send_method));
  buffer.push_back(send_param_1);
  buffer.push_back(send_param_2);
  buffer.push_back(static_cast<uint8_t>(src_node_type));
  buffer.push_back(static_cast<uint8_t>(msg_type));
  buffer.push_back(packet_num);
  buffer.push_back(data_len);
  // Write packet
  buffer.insert(buffer.end(), data, data + data_len);
  // Calculate CRC
  uint16_t crc = calculate_crc_(buffer.data(), PACKET_HEADER_SIZE + data_len);
  buffer.push_back((crc >> 8) & 0xFF);
  buffer.push_back(crc & 0xFF);

  if (queue_send) {
    message_queued_ = require_arbitration ? QueuedMessageType::ARBITRATION : QueuedMessageType::NORMAL;
    if (message_queued_ == QueuedMessageType::ARBITRATION) {
      slot_delay_ = generate_slot_delay_();
      ESP_LOGI(TAG, "Will arbitrate with slot delay of %u", static_cast<unsigned int>(slot_delay_));
    }
  }
}

void Comfortnet::read_buffer_(int bytes_available, uint32_t now) {
  uint8_t bytes[bytes_available];

  if (!this->read_array(bytes, bytes_available)) {
    return;
  }

  for (int i = 0; i < bytes_available; i++) {
    rx_message_.push_back(bytes[i]);

    if (rx_message_.size() > PAYLOAD_LENGTH_POS &&
        rx_message_.size() == PACKET_HEADER_SIZE + rx_message_[PAYLOAD_LENGTH_POS] + PACKET_CRC_SIZE) {
      // We have a full message
      this->handle_message_(false, now);
      rx_message_.clear();
    }
  }
}

void Comfortnet::loop() {
#ifdef ARDUINO
  const uint32_t now = millis();
#else
  const uint32_t now = (uint32_t) (esp_timer_get_time() / 1000);
#endif
  if (!polling_due_ && update_interval_millis_ > 0 &&
      now - last_poll_enqueue_time_ >= update_interval_millis_) {
    polling_due_ = true;
  }
  if (node_id_ != static_cast<NodeAddress>(0) && !this->direct_poll_mode_ &&
      (pending_messages_.size() > 0 || (polling_due_ && !polling_queue_.empty())) &&
      now - this->last_r2r_time_ >= R2R_FALLBACK_TIMEOUT) {
    this->direct_poll_mode_ = true;
    ESP_LOGW(TAG, "No R2R for %u ms with %u queued message(s); enabling direct-poll fallback",
             static_cast<unsigned int>(now - this->last_r2r_time_),
             static_cast<unsigned int>(pending_messages_.size()));
  }
  if (this->direct_poll_mode_) {
    queue_next_poll_(now, true);
  }
  if (this->direct_poll_command_waiting_response_ &&
      now - this->direct_poll_command_tx_time_ >= REQUEST_TIMEOUT) {
    this->direct_poll_command_waiting_response_ = false;
    ESP_LOGW(TAG, "Direct-poll fallback command response timed out; command remains queued");
  }

  if (message_queued_ != QueuedMessageType::NONE) {
    uint32_t delay = message_queued_ == QueuedMessageType::ARBITRATION ? this->slot_delay_ : MINIMUM_SLOT_DELAY;
    if (now - this->last_read_time_ > delay) {
      if (this->available() > 0) {
        // Final check if line is busy
        message_queued_ = QueuedMessageType::NONE;
        slot_delay_ = 0;
        this->direct_poll_command_waiting_tx_ = false;
        tx_message_.clear();
        if (awaiting_discovery_) {
          session_id_.clear();
        }
        awaiting_discovery_ = false;
      } else {
        if (this->flow_control_pin_ != nullptr) {
          this->flow_control_pin_->digital_write(true);
        }
        this->write_array(tx_message_.data(), tx_message_.size());
        this->flush();
        if (this->flow_control_pin_ != nullptr) {
          this->flow_control_pin_->digital_write(false);
        }
        this->handle_message_(true, now);
        message_queued_ = QueuedMessageType::NONE;
        slot_delay_ = 0;
        tx_message_.clear();
      }
    }
  }

  // Read Everything that is in the buffer
  int bytes_available = this->available();
  if (bytes_available > 0) {
    this->last_read_time_ = now;
    this->read_buffer_(bytes_available, now);
  } else if (node_id_ != static_cast<NodeAddress>(0) && now - this->last_read_time_ > SILENCE_TIMEOUT) {
    ESP_LOGW(TAG, "Network appears to be offline");
    disconnect_();
    // Disconnect
  } else if ((now - this->last_read_time_ > RECEIVE_TIMEOUT) && !rx_message_.empty()) {
    ESP_LOGW(TAG, "Timed out reading partial message");
    rx_message_.clear();
  }
  if (node_id_ != static_cast<NodeAddress>(0) && (now - this->last_address_confirm_time_ > NETWORK_TIMEOUT)) {
    ESP_LOGW(TAG, "Dropped from network, discarding session information");
    disconnect_();
  }
}

void Comfortnet::disconnect_() {
  rx_message_.clear();
  tx_message_.clear();
  r2r_reply_.clear();
  message_queued_ = QueuedMessageType::NONE;
  slot_delay_ = 0;
  last_r2r_time_ = 0;
  direct_poll_mode_ = false;
  direct_poll_command_waiting_tx_ = false;
  direct_poll_command_waiting_response_ = false;
  user_menu_probe_started_ = false;
  user_menu_probe_index_ = 0;
  direct_poll_command_tx_time_ = 0;
  awaiting_discovery_ = false;
  has_won_token_broadcast_ = false;
  polling_due_ = true;
  last_poll_enqueue_time_ = 0;

  node_id_ = static_cast<NodeAddress>(0);
  subnet_ = Subnet::BROADCAST;
  session_id_.clear();
  call_listener_(DATA_KEY_NETWORK_STATUS,
                 (struct ComfortnetData) {this->device_type_, ComfortnetData::DataType::BOOLEAN, false});
}

}  // namespace comfortnet
