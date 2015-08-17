#include <syslog.h>
#include <xpc/xpc.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <signal.h>

#import <ServiceManagement/ServiceManagement.h>
#import <Security/Authorization.h>

IOHIDManagerRef hidManager;
IOHIDDeviceRef hidDevice;

static void match_set(CFMutableDictionaryRef dict, CFStringRef key, int value) {
  CFNumberRef number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
  CFDictionarySetValue(dict, key, number);
  CFRelease(number);
}

static void handle_removal_callback(void *context, IOReturn result,
                                    void *sender, IOHIDDeviceRef device) {
  if (hidDevice != NULL) {
    IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
    hidDevice = NULL;
  }
  if (hidManager != NULL) {
    IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
    hidManager = NULL;
  }
}

static void match_callback(void *context, IOReturn result, void *sender,
                           IOHIDDeviceRef device) {
  IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
  if (r == kIOReturnSuccess) {
    syslog(LOG_NOTICE, "Open'ed HID device");
    hidDevice = device;
  } else {
    syslog(LOG_ALERT, "Failed to open HID device");
  }
}

static CFDictionaryRef matching_dictionary_create(int vendorID, int productID,
                                                  int usagePage, int usage) {
  CFMutableDictionaryRef match = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);

  if (vendorID) {
    match_set(match, CFSTR(kIOHIDVendorIDKey), vendorID);
  }
  if (productID) {
    match_set(match, CFSTR(kIOHIDProductIDKey), productID);
  }
  if (usagePage) {
    match_set(match, CFSTR(kIOHIDDeviceUsagePageKey), usagePage);
  }
  if (usage) {
    match_set(match, CFSTR(kIOHIDDeviceUsageKey), usage);
  }

  return match;
}

static void __XPC_Peer_Event_Handler(xpc_connection_t connection,
                                     xpc_object_t event) {
  xpc_type_t type = xpc_get_type(event);

  if (type == XPC_TYPE_ERROR) {
    if (event == XPC_ERROR_CONNECTION_INVALID) {
      // The client process on the other end of the connection has either
      // crashed or cancelled the connection. After receiving this error,
      // the connection is in an invalid state, and you do not need to
      // call xpc_connection_cancel(). Just tear down any associated state
      // here.
    } else if (event == XPC_ERROR_TERMINATION_IMMINENT) {
      // Handle per-connection termination cleanup.
    }

  } else {
    uint64_t idProduct = xpc_dictionary_get_int64(event, "idProduct");
    uint64_t idVendor = xpc_dictionary_get_int64(event, "idVendor");
    uint64_t action = xpc_dictionary_get_int64(event, "request");
    syslog(LOG_NOTICE,
           "Received message. idProduct: %llu, idVendor: %llu, action: %llu",
           idProduct, idVendor, action);
    if (action == 1) {
      // enable
      if (hidDevice != NULL) {
        IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
        hidDevice = NULL;
      }
      if (hidManager != NULL) {
        IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
        hidManager = NULL;
      }
    } else {
      // disable
      if (hidManager == NULL) {
        hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        IOHIDManagerRegisterDeviceMatchingCallback(hidManager, match_callback, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(hidManager, handle_removal_callback, NULL);
      }
      IOHIDManagerScheduleWithRunLoop(hidManager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
      CFDictionaryRef match = matching_dictionary_create(idVendor, idProduct, 1, 6);
      IOHIDManagerSetDeviceMatching(hidManager, match);
      CFRelease(match);
    }
    xpc_connection_t remote = xpc_dictionary_get_remote_connection(event);
    xpc_object_t reply = xpc_dictionary_create_reply(event);
    xpc_dictionary_set_string(reply, "reply", "OK");
    xpc_connection_send_message(remote, reply);
    xpc_release(reply);
  }
}

static void __XPC_Connection_Handler(xpc_connection_t connection) {
  xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
    __XPC_Peer_Event_Handler(connection, event);
  });

  xpc_connection_resume(connection);
}

void signalHandler(int signum) {
  syslog(LOG_NOTICE, "Received signal %d. Cleaning up...", signum);
  if (hidDevice != NULL) {
    IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeSeizeDevice);
    hidDevice = NULL;
  }
  if (hidManager != NULL) {
    IOHIDManagerClose(hidManager, kIOHIDOptionsTypeNone);
    hidManager = NULL;
  }
}

int main(int argc, const char *argv[]) {
  signal(SIGINT, signalHandler);
  xpc_connection_t service = xpc_connection_create_mach_service(
      "com.pallotron.yubiswitch.helper", dispatch_get_main_queue(),
      XPC_CONNECTION_MACH_SERVICE_LISTENER);

  if (!service) {
    syslog(LOG_CRIT, "Failed to create service.");
    exit(EXIT_FAILURE);
  }

  syslog(LOG_NOTICE, "Configuring connection event handler for helper");
  xpc_connection_set_event_handler(service, ^(xpc_object_t connection) {
    __XPC_Connection_Handler(connection);
  });

  xpc_connection_resume(service);
  CFRunLoopRun();
  dispatch_main();
  return EXIT_SUCCESS;
}
