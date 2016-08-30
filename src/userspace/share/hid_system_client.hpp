#pragma once

#include "boost_defs.hpp"

#include "iokit_utility.hpp"
#include "service_observer.hpp"
#include "userspace_types.hpp"
#include <boost/optional.hpp>
#include <list>
#include <memory>

class hid_system_client final {
public:
  // Note:
  // OS X shares IOHIDSystem among all input devices even the serial_number of IOHIDSystem is same with the one of the input device.
  //
  // Example:
  //   The matched_callback always contains only one IOHIDSystem even if the following devices are connected.
  //     * Apple Internal Keyboard / Track
  //     * HHKB-BT
  //     * org.pqrs.driver.VirtualHIDKeyboard
  //
  //   The IOHIDSystem object's serial_number is one of the connected devices.
  //
  //   But the IOHIDSystem object is shared by all input devices.
  //   Thus, the IOHIDGetModifierLockState returns true if caps lock is on in one device.

  hid_system_client(spdlog::logger& logger) : logger_(logger),
                                              service_(IO_OBJECT_NULL),
                                              connect_(IO_OBJECT_NULL) {
    matching_dictionary_ = IOServiceNameMatching(kIOHIDSystemClass);
    if (!matching_dictionary_) {
      logger_.error("IOServiceNameMatching error @ {0}", __PRETTY_FUNCTION__);
    } else {
      service_observer_ = std::make_unique<service_observer>(logger_,
                                                             matching_dictionary_,
                                                             std::bind(&hid_system_client::matched_callback, this, std::placeholders::_1),
                                                             std::bind(&hid_system_client::terminated_callback, this, std::placeholders::_1));
    }
  }

  ~hid_system_client(void) {
    close_connection();

    if (matching_dictionary_) {
      CFRelease(matching_dictionary_);
    }
  }

  void post_modifier_flags(IOOptionBits flags) {
    NXEventData event;
    memset(&event, 0, sizeof(event));

    IOGPoint loc{0, 0};
    post_event(NX_FLAGSCHANGED, loc, &event, kNXEventDataVersion, flags, kIOHIDSetGlobalEventFlags);
  }

  enum class post_key_type {
    key,
    aux_control_button,
  };

  void post_key(post_key_type type, uint8_t key_code, krbn::event_type event_type, IOOptionBits flags, bool repeat) {
    switch (type) {
    case post_key_type::key:
      post_key(key_code, event_type, flags, repeat);
      break;

    case post_key_type::aux_control_button:
      post_aux_control_button(key_code, event_type, flags, repeat);
      break;
    }
  }

  void post_key(uint8_t key_code, krbn::event_type event_type, IOOptionBits flags, bool repeat) {
    NXEventData event;
    memset(&event, 0, sizeof(event));
    event.key.origCharCode = 0;
    event.key.repeat = repeat;
    event.key.charSet = NX_ASCIISET;
    event.key.charCode = 0;
    event.key.keyCode = key_code;
    event.key.origCharSet = NX_ASCIISET;
    event.key.keyboardType = 0;

    IOGPoint loc{0, 0};
    post_event(event_type == krbn::event_type::key_down ? NX_KEYDOWN : NX_KEYUP,
               loc,
               &event,
               kNXEventDataVersion,
               flags,
               0);
  }

  void post_aux_control_button(uint8_t key_code, krbn::event_type event_type, IOOptionBits flags, bool repeat) {
    NXEventData event;
    memset(&event, 0, sizeof(event));
    event.compound.subType = NX_SUBTYPE_AUX_CONTROL_BUTTONS;
    event.compound.misc.L[0] = (key_code << 16) | ((event_type == krbn::event_type::key_down ? NX_KEYDOWN : NX_KEYUP) << 8) | repeat;

    IOGPoint loc{0, 0};
    post_event(NX_SYSDEFINED, loc, &event, kNXEventDataVersion, flags, 0);
  }

  boost::optional<bool> get_caps_lock_state(void) const {
    return get_modifier_lock_state(kIOHIDCapsLockState);
  }

  bool set_caps_lock_state(bool state) {
    return set_modifier_lock_state(kIOHIDCapsLockState, state);
  }

private:
  void matched_callback(io_iterator_t iterator) {
    while (auto service = IOIteratorNext(iterator)) {
      std::lock_guard<std::mutex> guard(mutex_);

      // Use first matched service.
      if (!service_) {
        service_ = service;
        IOObjectRetain(service_);

        auto kr = IOServiceOpen(service_, mach_task_self(), kIOHIDParamConnectType, &connect_);
        if (kr != KERN_SUCCESS) {
          logger_.error("IOServiceOpen error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, kr);
          connect_ = IO_OBJECT_NULL;
        }

        logger_.info("IOServiceOpen is succeeded @ {0}", __PRETTY_FUNCTION__);
      }

      IOObjectRelease(service);
    }
  }

  void terminated_callback(io_iterator_t iterator) {
    bool found = false;

    while (auto service = IOIteratorNext(iterator)) {
      found = true;
      IOObjectRelease(service);
    }

    if (!found) {
      return;
    }

    // Refresh connection.
    {
      std::lock_guard<std::mutex> guard(mutex_);
      close_connection();
    }

    io_iterator_t it;
    auto kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching_dictionary_, &it);
    if (kr != KERN_SUCCESS) {
      logger_.error("IOServiceGetMatchingServices error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, kr);
    } else {
      matched_callback(it);
      IOObjectRelease(it);
    }
  }

  void post_event(UInt32 event_type,
                  IOGPoint location,
                  const NXEventData* _Nullable event_data,
                  UInt32 event_data_version,
                  IOOptionBits event_flags,
                  IOOptionBits options) {
    std::lock_guard<std::mutex> guard(mutex_);

    if (!connect_) {
      logger_.error("connect_ is null @ {0}", __PRETTY_FUNCTION__);
      return;
    }

    auto kr = IOHIDPostEvent(connect_, event_type, location, event_data, event_data_version, event_flags, options);
    if (KERN_SUCCESS != kr) {
      logger_.error("IOHIDPostEvent error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, kr);
    }
  }

  boost::optional<bool> get_modifier_lock_state(int selector) const {
    if (!connect_) {
      logger_.error("connect_ is null @ {0}", __PRETTY_FUNCTION__);
      return boost::none;
    }

    bool value;
    auto kr = IOHIDGetModifierLockState(connect_, selector, &value);
    if (KERN_SUCCESS != kr) {
      logger_.error("IOHIDGetModifierLockState error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, kr);
    }

    return value;
  }

  bool set_modifier_lock_state(int selector, bool state) {
    if (!connect_) {
      logger_.error("connect_ is null @ {0}", __PRETTY_FUNCTION__);
      return false;
    }

    auto kr = IOHIDSetModifierLockState(connect_, selector, state);
    if (KERN_SUCCESS != kr) {
      logger_.error("IOHIDSetModifierLockState error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, kr);
      return false;
    }

    return true;
  }

  void close_connection(void) {
    if (connect_) {
      auto kr = IOServiceClose(connect_);
      if (kr != kIOReturnSuccess) {
        logger_.error("IOConnectRelease error: 0x{1:x} @ {0}", __PRETTY_FUNCTION__, kr);
      }
      connect_ = IO_OBJECT_NULL;
    }

    logger_.info("IOServiceClose is succeeded @ {0}", __PRETTY_FUNCTION__);

    if (service_) {
      IOObjectRelease(service_);
    }
  }

  spdlog::logger& logger_;

  std::unique_ptr<service_observer> service_observer_;
  CFMutableDictionaryRef _Nullable matching_dictionary_;
  io_service_t service_;
  io_connect_t connect_;
  std::mutex mutex_;
};
