#include "protocol.h"

#include <assert.h>
#include <string.h>
#include "blapi/bootloader.h"
#include "byte_queue.h"
#include "features.h"
#include "pwm.h"
#include "adc.h"
#include "digital.h"
#include "logging.h"
#include "board.h"
#include "uart.h"

#define CHECK(cond) do { if (!(cond)) { log_printf("Check failed: %s", #cond); return FALSE; }} while(0)

#define FIRMWARE_ID              0x00000001LL

const BYTE incoming_arg_size[MESSAGE_TYPE_LIMIT] = {
  sizeof(HARD_RESET_ARGS),
  sizeof(SOFT_RESET_ARGS),
  sizeof(SET_PIN_DIGITAL_OUT_ARGS),
  sizeof(SET_DIGITAL_OUT_LEVEL_ARGS),
  sizeof(SET_PIN_DIGITAL_IN_ARGS),
  sizeof(SET_CHANGE_NOTIFY_ARGS),
  sizeof(REGISTER_PERIODIC_DIGITAL_SAMPLING_ARGS),
  sizeof(RESERVED_ARGS),
  sizeof(SET_PIN_PWM_ARGS),
  sizeof(SET_PWM_DUTY_CYCLE_ARGS),
  sizeof(SET_PWM_PERIOD_ARGS),
  sizeof(SET_PIN_ANALOG_IN_ARGS),
  sizeof(UART_DATA_ARGS),
  sizeof(UART_CONFIG_ARGS),
  sizeof(SET_PIN_UART_RX_ARGS),
  sizeof(SET_PIN_UART_TX_ARGS)
  // BOOKMARK(add_feature): Add sizeof (argument for incoming message).
  // Array is indexed by message type enum.
};

const BYTE outgoing_arg_size[MESSAGE_TYPE_LIMIT] = {
  sizeof(ESTABLISH_CONNECTION_ARGS),
  sizeof(SOFT_RESET_ARGS),
  sizeof(SET_PIN_DIGITAL_OUT_ARGS),
  sizeof(REPORT_DIGITAL_IN_STATUS_ARGS),
  sizeof(SET_PIN_DIGITAL_IN_ARGS),
  sizeof(SET_CHANGE_NOTIFY_ARGS),
  sizeof(REGISTER_PERIODIC_DIGITAL_SAMPLING_ARGS),
  sizeof(RESERVED_ARGS),
  sizeof(REPORT_ANALOG_IN_FORMAT_ARGS),
  sizeof(REPORT_ANALOG_IN_STATUS_ARGS),
  sizeof(UART_REPORT_TX_STATUS),
  sizeof(SET_PIN_ANALOG_IN_ARGS),
  sizeof(UART_DATA_ARGS),
  sizeof(UART_CONFIG_ARGS),
  sizeof(SET_PIN_UART_RX_ARGS),
  sizeof(SET_PIN_UART_TX_ARGS)
  // BOOKMARK(add_feature): Add sizeof (argument for outgoing message).
  // Array is indexed by message type enum.
};

DEFINE_STATIC_BYTE_QUEUE(tx_queue, 1024);
static int bytes_transmitted;

typedef enum {
  WAIT_TYPE,
  WAIT_ARGS,
  WAIT_VAR_ARGS
} RX_MESSAGE_STATE;

static INCOMING_MESSAGE rx_msg;
static int rx_buffer_cursor;
static int rx_message_remaining;
static RX_MESSAGE_STATE rx_message_state;

static inline BYTE OutgoingMessageLength(const OUTGOING_MESSAGE* msg) {
  return 1 + outgoing_arg_size[msg->type];
}

static inline BYTE IncomingVarArgSize(const INCOMING_MESSAGE* msg) {
  switch (msg->type) {
    case UART_DATA:
      return msg->args.uart_data.size + 1;

    // BOOKMARK(add_feature): Add more cases here if incoming message has variable args.
    default:
      return 0;
  }
}

void AppProtocolInit(ADB_CHANNEL_HANDLE h) {
  bytes_transmitted = 0;
  rx_buffer_cursor = 0;
  rx_message_remaining = 1;
  rx_message_state = WAIT_TYPE;
  ByteQueueClear(&tx_queue);

  OUTGOING_MESSAGE msg;
  msg.type = ESTABLISH_CONNECTION;
  msg.args.establish_connection.magic = IOIO_MAGIC;
  // TODO: read those from ROM somehow
  msg.args.establish_connection.hardware = 0;  // HardwareVer
  msg.args.establish_connection.bootloader = 1;  // BootloaderVer
  msg.args.establish_connection.firmware = FIRMWARE_ID;
  AppProtocolSendMessage(&msg);
}

void AppProtocolSendMessage(const OUTGOING_MESSAGE* msg) {
  BYTE lock;
  ByteQueueLock(lock, 1);
  ByteQueuePushBuffer(&tx_queue, (const BYTE*) msg, OutgoingMessageLength(msg));
  ByteQueueUnlock(lock);
}

void AppProtocolSendMessageWithVarArg(const OUTGOING_MESSAGE* msg, const void* data, int size) {
  BYTE lock;
  ByteQueueLock(lock, 1);
  ByteQueuePushBuffer(&tx_queue, (const BYTE*) msg, OutgoingMessageLength(msg));
  ByteQueuePushBuffer(&tx_queue, data, size);
  ByteQueueUnlock(lock);
}

void AppProtocolTasks(ADB_CHANNEL_HANDLE h) {
  UARTTasks();
  if (ADBChannelReady(h)) {
    BYTE lock;
    ByteQueueLock(lock, 1);
    const BYTE* data;
    int size;
    if (bytes_transmitted) {
      ByteQueuePull(&tx_queue, bytes_transmitted);
      bytes_transmitted = 0;
    }
    ByteQueuePeek(&tx_queue, &data, &size);
    if (size > 0) {
      ADBWrite(h, data, size);
      bytes_transmitted = size;
    }
    ByteQueueUnlock(lock);
  }
}

static void Echo() {
  AppProtocolSendMessage((const OUTGOING_MESSAGE*) &rx_msg);
}

static BOOL MessageDone() {
  // TODO: check pin capabilities
  switch (rx_msg.type) {
    case HARD_RESET:
      CHECK(rx_msg.args.hard_reset.magic == IOIO_MAGIC);
      HardReset();
      break;

    case SOFT_RESET:
      SoftReset();
      Echo();
      break;

    case SET_PIN_DIGITAL_OUT:
      CHECK(rx_msg.args.set_pin_digital_out.pin < NUM_PINS);
      SetPinDigitalOut(rx_msg.args.set_pin_digital_out.pin,
                       rx_msg.args.set_pin_digital_out.value,
                       rx_msg.args.set_pin_digital_out.open_drain);
      Echo();
      break;

    case SET_DIGITAL_OUT_LEVEL:
      CHECK(rx_msg.args.set_digital_out_level.pin < NUM_PINS);
      SetDigitalOutLevel(rx_msg.args.set_digital_out_level.pin,
                         rx_msg.args.set_digital_out_level.value);
      break;

    case SET_PIN_DIGITAL_IN:
      CHECK(rx_msg.args.set_pin_digital_in.pin < NUM_PINS);
      CHECK(rx_msg.args.set_pin_digital_in.pull < 3);
      SetPinDigitalIn(rx_msg.args.set_pin_digital_in.pin, rx_msg.args.set_pin_digital_in.pull);
      Echo();
      break;

    case SET_CHANGE_NOTIFY:
      CHECK(rx_msg.args.set_change_notify.pin < NUM_PINS);
      SetChangeNotify(rx_msg.args.set_change_notify.pin, rx_msg.args.set_change_notify.cn);
      Echo();
      if (rx_msg.args.set_change_notify.cn) {
        ReportDigitalInStatus(rx_msg.args.set_change_notify.pin);
      }
      break;

    case SET_PIN_PWM:
      CHECK(rx_msg.args.set_pin_pwm.pin < NUM_PINS);
      CHECK(rx_msg.args.set_pin_pwm.pwm_num < NUM_PWMS
            || rx_msg.args.set_pin_pwm.pwm_num == 0xF);
      SetPinPwm(rx_msg.args.set_pin_pwm.pin, rx_msg.args.set_pin_pwm.pwm_num);
      break;

    case SET_PWM_DUTY_CYCLE:
      CHECK(rx_msg.args.set_pwm_duty_cycle.pwm_num < NUM_PWMS);
      SetPwmDutyCycle(rx_msg.args.set_pwm_duty_cycle.pwm_num,
                      rx_msg.args.set_pwm_duty_cycle.dc,
                      rx_msg.args.set_pwm_duty_cycle.fraction);
      break;

    case SET_PWM_PERIOD:
      CHECK(rx_msg.args.set_pwm_period.pwm_num < NUM_PWMS);
      SetPwmPeriod(rx_msg.args.set_pwm_period.pwm_num,
                   rx_msg.args.set_pwm_period.period,
                   rx_msg.args.set_pwm_period.scale256);
      break;

    case SET_PIN_ANALOG_IN:
      CHECK(rx_msg.args.set_pin_analog_in.pin < NUM_PINS);
      SetPinAnalogIn(rx_msg.args.set_pin_analog_in.pin);
      Echo();
      break;

    case UART_DATA:
      CHECK(rx_msg.args.uart_data.uart_num < NUM_UARTS);
      UARTTransmit(rx_msg.args.uart_data.uart_num,
                   rx_msg.args.uart_data.data,
                   rx_msg.args.uart_data.size + 1);
      break;

    case UART_CONFIG:
      CHECK(rx_msg.args.uart_config.uart_num < NUM_UARTS);
      CHECK(rx_msg.args.uart_config.parity < 3);
      UARTConfig(rx_msg.args.uart_config.uart_num,
                 rx_msg.args.uart_config.rate,
                 rx_msg.args.uart_config.speed4x,
                 rx_msg.args.uart_config.two_stop_bits,
                 rx_msg.args.uart_config.parity);
      Echo();
      UARTReportTxStatus(rx_msg.args.uart_config.uart_num);
      break;

    case SET_PIN_UART_RX:
      CHECK(rx_msg.args.set_pin_uart_rx.pin < NUM_PINS);
      CHECK(rx_msg.args.set_pin_uart_rx.uart_num < NUM_UARTS);
      SetPinUartRx(rx_msg.args.set_pin_uart_rx.pin,
                   rx_msg.args.set_pin_uart_rx.uart_num,
                   rx_msg.args.set_pin_uart_rx.enable);
      Echo();
      break;

    case SET_PIN_UART_TX:
      CHECK(rx_msg.args.set_pin_uart_tx.pin < NUM_PINS);
      CHECK(rx_msg.args.set_pin_uart_tx.uart_num < NUM_UARTS);
      SetPinUartTx(rx_msg.args.set_pin_uart_tx.pin,
                   rx_msg.args.set_pin_uart_tx.uart_num,
                   rx_msg.args.set_pin_uart_tx.enable);
      Echo();
      break;

    // BOOKMARK(add_feature): Add incoming message handling to switch clause.
    // Call Echo() if the message is to be echoed back.

    default:
      return FALSE;
  }
  return TRUE;
}

BOOL AppProtocolHandleIncoming(const BYTE* data, UINT32 data_len) {
  assert(data);

  while (data_len > 0) {
    // copy a chunk of data to rx_msg
    if (data_len >= rx_message_remaining) {
      memcpy(((BYTE *) &rx_msg) + rx_buffer_cursor, data, rx_message_remaining);
      data += rx_message_remaining;
      data_len -= rx_message_remaining;
      rx_buffer_cursor += rx_message_remaining;
      rx_message_remaining = 0;
    } else {
      memcpy(((BYTE *) &rx_msg) + rx_buffer_cursor, data, data_len);
      rx_buffer_cursor += data_len;
      rx_message_remaining -= data_len;
      data_len = 0;
    }

    // change state
    if (rx_message_remaining == 0) {
      switch (rx_message_state) {
        case WAIT_TYPE:
          rx_message_state = WAIT_ARGS;
          rx_message_remaining = incoming_arg_size[rx_msg.type];
          if (rx_message_remaining) break;
          // fall-through on purpose

        case WAIT_ARGS:
          rx_message_state = WAIT_VAR_ARGS;
          rx_message_remaining = IncomingVarArgSize(&rx_msg);
          if (rx_message_remaining) break;
          // fall-through on purpose

        case WAIT_VAR_ARGS:
          rx_message_state = WAIT_TYPE;
          rx_message_remaining = 1;
          rx_buffer_cursor = 0;
          if (!MessageDone()) return FALSE;
          break;
      }
    }
  }
  return TRUE;
}
