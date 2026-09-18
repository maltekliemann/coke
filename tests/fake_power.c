/* Run the real CLI, locks, spawn and signal handling without changing power
 * settings. Only the framework boundary is replaced. */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibDefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/ps/IOPowerSources.h>

static bool option(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] == '1';
}

static void trace(const char *event)
{
    const char *path = getenv("COKE_TEST_TRACE");
    if (!path)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        abort();
    char line[128];
    int n = snprintf(line, sizeof(line), "%d %s\n", getpid(), event);
    if (write(fd, line, (size_t)n) != n)
        abort();
    close(fd);
}

static io_service_t fake_service(mach_port_t port, CFDictionaryRef match)
{
    (void)port;
    CFRelease(match);
    return 42;
}

static kern_return_t fake_open(io_service_t service, task_port_t task,
                               uint32_t type, io_connect_t *out)
{
    (void)service;
    (void)task;
    (void)type;
    *out = 43;
    return KERN_SUCCESS;
}

static kern_return_t fake_release(io_object_t object)
{
    (void)object;
    return KERN_SUCCESS;
}

static kern_return_t fake_close(io_connect_t connection)
{
    (void)connection;
    return KERN_SUCCESS;
}

static kern_return_t fake_scalar(mach_port_t connection, uint32_t selector,
    const uint64_t *input, uint32_t count, uint64_t *output, uint32_t *output_count)
{
    (void)connection;
    (void)output;
    (void)output_count;
    static int sets;
    if (selector != kPMSetClamshellSleepState || count != 1)
        abort();
    if (*input) {
        sets++;
        if (option("COKE_TEST_FAIL_SET") ||
            (sets > 1 && option("COKE_TEST_FAIL_RETRY"))) {
            trace("SET_FAILED");
            return kIOReturnError;
        }
        trace("SET");
    } else {
        if (option("COKE_TEST_FAIL_CLEAR")) {
            trace("CLEAR_FAILED");
            return kIOReturnError;
        }
        trace("CLEAR");
    }
    return kIOReturnSuccess;
}

static CFTypeRef fake_property(io_registry_entry_t entry, CFStringRef key,
                              CFAllocatorRef allocator, IOOptionBits options)
{
    (void)entry;
    (void)allocator;
    (void)options;
    if (option("COKE_TEST_MISSING_PROPERTY"))
        return NULL;
    return CFRetain(CFEqual(key, CFSTR("AppleClamshellState")) ?
                    kCFBooleanFalse : kCFBooleanTrue);
}

static CGError fake_displays(uint32_t capacity, CGDirectDisplayID *ids, uint32_t *count)
{
    if (option("COKE_TEST_UNKNOWN_DISPLAY"))
        return kCGErrorFailure;
    *count = 1;
    if (capacity)
        ids[0] = 1;
    return kCGErrorSuccess;
}

static uint32_t fake_builtin(CGDirectDisplayID id)
{
    (void)id;
    return !option("COKE_TEST_EXTERNAL");
}

static CFTypeRef fake_power_info(void)
{
    return option("COKE_TEST_UNKNOWN_POWER") ? NULL : CFRetain(CFSTR("fake"));
}

static CFStringRef fake_power_type(CFTypeRef info)
{
    (void)info;
    return option("COKE_TEST_AC") ? CFSTR(kIOPMACPowerKey) : CFSTR(kIOPMBatteryPowerKey);
}

static IOReturn fake_assertions(CFDictionaryRef *out)
{
    if (option("COKE_TEST_UNKNOWN_ASSERTIONS"))
        return kIOReturnError;
    CFMutableDictionaryRef assertions = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (option("COKE_TEST_LID_ASSERTION") || option("COKE_TEST_HOTPLUG_ASSERTION")) {
        int level = option("COKE_TEST_INACTIVE_ASSERTION") ? 0 : 1;
        CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &level);
        const void *keys[] = { kIOPMAssertionLevelKey,
            option("COKE_TEST_HOTPLUG_ASSERTION") ? CFSTR("ProcessingHotPlug") : CFSTR("AppliesOnLidClose") };
        const void *values[] = { number, kCFBooleanTrue };
        CFDictionaryRef assertion = CFDictionaryCreate(NULL, keys, values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const void *items[] = { assertion };
        CFArrayRef array = CFArrayCreate(NULL, items, 1, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(assertions, CFSTR("test-pid"), array);
        CFRelease(array);
        CFRelease(assertion);
        CFRelease(number);
    }
    *out = assertions;
    return kIOReturnSuccess;
}

static IOReturn fake_idle_create(CFStringRef type, IOPMAssertionLevel level,
                                CFStringRef name, IOPMAssertionID *out)
{
    (void)type;
    (void)level;
    (void)name;
    if (option("COKE_TEST_FAIL_IDLE"))
        return kIOReturnError;
    *out = 1;
    trace("IDLE_ON");
    return kIOReturnSuccess;
}

static IOReturn fake_idle_release(IOPMAssertionID id)
{
    (void)id;
    if (option("COKE_TEST_FAIL_IDLE_RELEASE"))
        return kIOReturnError;
    trace("IDLE_OFF");
    return kIOReturnSuccess;
}

#define IOServiceGetMatchingService fake_service
#define IOServiceOpen fake_open
#define IOObjectRelease fake_release
#define IOServiceClose fake_close
#define IOConnectCallScalarMethod fake_scalar
#define IORegistryEntryCreateCFProperty fake_property
#define CGGetOnlineDisplayList fake_displays
#define CGDisplayIsBuiltin fake_builtin
#define IOPSCopyPowerSourcesInfo fake_power_info
#define IOPSGetProvidingPowerSourceType fake_power_type
#define IOPMCopyAssertionsByProcess fake_assertions
#define IOPMAssertionCreateWithName fake_idle_create
#define IOPMAssertionRelease fake_idle_release
#define main coke_main
#include "../coke.c"
#undef main

int main(int argc, char **argv)
{
    control_path = getenv("COKE_TEST_CONTROL");
    sessions_path = getenv("COKE_TEST_SESSIONS");
    if (!control_path || !sessions_path)
        return 99;
    return coke_main(argc, argv);
}
