#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "cfg.h"
#include "dbg.h"

#include "test.h"


static void
verifyDefaults(config_t* config)
{
    assert_int_equal       (cfgOutFormat(config), DEFAULT_OUT_FORMAT);
    assert_string_equal    (cfgOutStatsDPrefix(config), DEFAULT_STATSD_PREFIX);
    assert_int_equal       (cfgOutStatsDMaxLen(config), DEFAULT_STATSD_MAX_LEN);
    assert_int_equal       (cfgOutVerbosity(config), DEFAULT_OUT_VERBOSITY);
    assert_int_equal       (cfgOutPeriod(config), DEFAULT_SUMMARY_PERIOD);
    assert_string_equal    (cfgCmdDir(config), DEFAULT_COMMAND_DIR);
    assert_int_equal       (cfgEventFormat(config), DEFAULT_EVT_FORMAT);
    assert_string_equal    (cfgEventLogFileFilter(config), DEFAULT_LOG_FILE_FILTER);
    assert_int_equal       (cfgEventSource(config, CFG_SRC_LOGFILE), DEFAULT_SRC_LOGFILE);
    assert_int_equal       (cfgEventSource(config, CFG_SRC_LOGFILE), DEFAULT_SRC_CONSOLE);
    assert_int_equal       (cfgEventSource(config, CFG_SRC_LOGFILE), DEFAULT_SRC_SYSLOG);
    assert_int_equal       (cfgEventSource(config, CFG_SRC_LOGFILE), DEFAULT_SRC_METRIC);
    assert_int_equal       (cfgTransportType(config, CFG_OUT), CFG_UDP);
    assert_string_equal    (cfgTransportHost(config, CFG_OUT), "127.0.0.1");
    assert_string_equal    (cfgTransportPort(config, CFG_OUT), DEFAULT_OUT_PORT);
    assert_null            (cfgTransportPath(config, CFG_OUT));
    assert_int_equal       (cfgTransportBuf(config, CFG_OUT), CFG_BUFFER_FULLY);
    assert_int_equal       (cfgTransportType(config, CFG_EVT), CFG_UDP);
    assert_string_equal    (cfgTransportHost(config, CFG_EVT), "127.0.0.1");
    assert_string_equal    (cfgTransportPort(config, CFG_EVT), DEFAULT_EVT_PORT);
    assert_null            (cfgTransportPath(config, CFG_EVT));
    assert_int_equal       (cfgTransportBuf(config, CFG_EVT), CFG_BUFFER_FULLY);
    assert_int_equal       (cfgTransportType(config, CFG_LOG), CFG_FILE);
    assert_null            (cfgTransportHost(config, CFG_LOG));
    assert_null            (cfgTransportPort(config, CFG_LOG));
    assert_string_equal    (cfgTransportPath(config, CFG_LOG), "/tmp/scope.log");
    assert_int_equal       (cfgTransportBuf(config, CFG_OUT), CFG_BUFFER_FULLY);
    assert_null            (cfgCustomTags(config));
    assert_null            (cfgCustomTagValue(config, "tagname"));
    assert_int_equal       (cfgLogLevel(config), DEFAULT_LOG_LEVEL);
}

static void
cfgCreateDefaultReturnsValidPtr(void** state)
{
    // construction returns non-null ptr
    config_t* config = cfgCreateDefault();
    assert_non_null (config);

    // cleanup
    cfgDestroy(&config);
    assert_null(config);
}

static void
accessorValuesForDefaultConfigAreAsExpected(void** state)
{
    // defaults via accessors of the config object
    config_t* config = cfgCreateDefault();
    verifyDefaults(config);

    // cleanup
    cfgDestroy(&config);
    assert_null(config);
}

static void
accessorsReturnDefaultsWhenConfigIsNull(void** state)
{
    // Implicitly this verifies no crashes when trying to access a null object
    verifyDefaults(NULL);
}

static void
cfgOutFormatSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgOutFormatSet(config, CFG_METRIC_JSON);
    assert_int_equal(cfgOutFormat(config), CFG_METRIC_JSON);
    cfgOutFormatSet(config, CFG_METRIC_STATSD);
    assert_int_equal(cfgOutFormat(config), CFG_METRIC_STATSD);
    cfgDestroy(&config);
}

static void
cfgOutStatsDPrefixSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgOutStatsDPrefixSet(config, "heywithdot.");
    assert_string_equal(cfgOutStatsDPrefix(config), "heywithdot.");
    cfgOutStatsDPrefixSet(config, "heywithoutdot");
    assert_string_equal(cfgOutStatsDPrefix(config), "heywithoutdot.");
    cfgOutStatsDPrefixSet(config, NULL);
    assert_string_equal(cfgOutStatsDPrefix(config), DEFAULT_STATSD_PREFIX);
    cfgOutStatsDPrefixSet(config, "");
    assert_string_equal(cfgOutStatsDPrefix(config), "");
    cfgDestroy(&config);
}

static void
cfgOutStatsDMaxLenSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgOutStatsDMaxLenSet(config, 0);
    assert_int_equal(cfgOutStatsDMaxLen(config), 0);
    cfgOutStatsDMaxLenSet(config, UINT_MAX);
    assert_int_equal(cfgOutStatsDMaxLen(config), UINT_MAX);
    cfgDestroy(&config);
}

static void
cfgOutVerbositySetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    int i;
    for (i=0; i<=CFG_MAX_VERBOSITY+1; i++) {
        cfgOutVerbositySet(config, i);
        if (i > CFG_MAX_VERBOSITY)
            assert_int_equal(cfgOutVerbosity(config), CFG_MAX_VERBOSITY);
        else
            assert_int_equal(cfgOutVerbosity(config), i);
    }
    cfgOutVerbositySet(config, UINT_MAX);
    assert_int_equal(cfgOutVerbosity(config), CFG_MAX_VERBOSITY);
    cfgDestroy(&config);
}

static void
cfgOutPeriodSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgOutPeriodSet(config, 0);
    assert_int_equal(cfgOutPeriod(config), 0);
    cfgOutPeriodSet(config, UINT_MAX);
    assert_int_equal(cfgOutPeriod(config), UINT_MAX);
    cfgDestroy(&config);
}

static void
cfgCmdDirSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgCmdDirSet(config, "/some/path");
    assert_string_equal(cfgCmdDir(config), "/some/path");
    cfgCmdDirSet(config, NULL);
    assert_string_equal(cfgCmdDir(config), DEFAULT_COMMAND_DIR);
    cfgDestroy(&config);
}

static void
cfgEventFormatSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgEventFormatSet(config, CFG_METRIC_STATSD);
    assert_int_equal(cfgEventFormat(config), CFG_METRIC_STATSD);
    cfgEventFormatSet(config, CFG_METRIC_JSON);
    assert_int_equal(cfgEventFormat(config), CFG_METRIC_JSON);
    cfgDestroy(&config);
}

static void
cfgEventLogFileFilterSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgEventLogFileFilterSet(config, ".*\\.log$");
    assert_string_equal(cfgEventLogFileFilter(config), ".*\\.log$");
    cfgEventLogFileFilterSet(config, "^/var/log/.*");
    assert_string_equal(cfgEventLogFileFilter(config), "^/var/log/.*");
    cfgEventLogFileFilterSet(config, NULL);
    assert_string_equal(cfgEventLogFileFilter(config), DEFAULT_LOG_FILE_FILTER);
    cfgDestroy(&config);
}

static void
cfgEventSourceSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();

    // Set everything to 1
    int i, j;
    for (i=CFG_SRC_LOGFILE; i<CFG_SRC_MAX+1; i++) {
        cfgEventSourceSet(config, i, 1);
        if (i >= CFG_SRC_MAX) {
             assert_int_equal(cfgEventSource(config, i), DEFAULT_SRC_LOGFILE);
             assert_int_equal(dbgCountMatchingLines("src/cfg.c"), 1);
             dbgInit(); // reset dbg for the rest of the tests
        } else {
             assert_int_equal(dbgCountMatchingLines("src/cfg.c"), 0);
             assert_int_equal(cfgEventSource(config, i), 1);
        }
    }

    // Clear one at a time to see there aren't side effects
    for (i=CFG_SRC_LOGFILE; i<CFG_SRC_MAX; i++) {
        cfgEventSourceSet(config, i, 0); // Clear it
        for (j=CFG_SRC_LOGFILE; j<CFG_SRC_MAX; j++) {
            if (i==j)
                 assert_int_equal(cfgEventSource(config, j), 0);
            else
                 assert_int_equal(cfgEventSource(config, j), 1);
        }
        cfgEventSourceSet(config, i, 1); // Set it back
    }

    cfgDestroy(&config);

    // Test get with NULL config
    for (i=CFG_SRC_LOGFILE; i<CFG_SRC_MAX; i++) {
        unsigned expected;
        switch (i) {
            case CFG_SRC_LOGFILE:
                expected = DEFAULT_SRC_LOGFILE;
                break;
            case CFG_SRC_CONSOLE:
                expected = DEFAULT_SRC_CONSOLE;
                break;
            case CFG_SRC_SYSLOG:
                expected = DEFAULT_SRC_SYSLOG;
                break;
            case CFG_SRC_METRIC:
                expected = DEFAULT_SRC_METRIC;
                break;
        }
        assert_int_equal(cfgEventSource(config, i), expected);
    }
}

static void
cfgTransportTypeSetAndGet(void** state)
{
    which_transport_t t = *(which_transport_t*)state[0];
    config_t* config = cfgCreateDefault();
    cfgTransportTypeSet(config, t, CFG_UDP);
    assert_int_equal(cfgTransportType(config, t), CFG_UDP);
    cfgTransportTypeSet(config, t, CFG_UNIX);
    assert_int_equal(cfgTransportType(config, t), CFG_UNIX);
    cfgTransportTypeSet(config, t, CFG_FILE);
    assert_int_equal(cfgTransportType(config, t), CFG_FILE);
    cfgTransportTypeSet(config, t, CFG_SYSLOG);
    assert_int_equal(cfgTransportType(config, t), CFG_SYSLOG);
    cfgTransportTypeSet(config, t, CFG_SHM);
    assert_int_equal(cfgTransportType(config, t), CFG_SHM);
    cfgDestroy(&config);
}

static void
cfgTransportHostSetAndGet(void** state)
{
    which_transport_t t = *(which_transport_t*)state[0];
    config_t* config = cfgCreateDefault();
    cfgTransportHostSet(config, t, "larrysComputer");
    assert_string_equal(cfgTransportHost(config, t), "larrysComputer");
    cfgTransportHostSet(config, t, "bobsComputer");
    assert_string_equal(cfgTransportHost(config, t), "bobsComputer");
    cfgTransportHostSet(config, t, NULL);
    assert_null(cfgTransportHost(config, t));
    cfgDestroy(&config);
}

static void
cfgTransportPortSetAndGet(void** state)
{
    which_transport_t t = *(which_transport_t*)state[0];
    config_t* config = cfgCreateDefault();
    cfgTransportPortSet(config, t, "54321");
    assert_string_equal(cfgTransportPort(config, t), "54321");
    cfgTransportPortSet(config, t, "12345");
    assert_string_equal(cfgTransportPort(config, t), "12345");
    cfgDestroy(&config);
}

static void
cfgTransportPathSetAndGet(void** state)
{
    which_transport_t t = *(which_transport_t*)state[0];
    config_t* config = cfgCreateDefault();
    cfgTransportPathSet(config, t, "/tmp/mysock");
    assert_string_equal(cfgTransportPath(config, t), "/tmp/mysock");
    cfgTransportPathSet(config, t, NULL);
    assert_null(cfgTransportPath(config, t));
    cfgDestroy(&config);
}

static void
cfgTransportBufSetAndGet(void** state)
{
    which_transport_t t = *(which_transport_t*)state[0];
    config_t* config = cfgCreateDefault();
    cfgTransportBufSet(config, t, CFG_BUFFER_LINE);
    assert_int_equal(cfgTransportBuf(config, t), CFG_BUFFER_LINE);
    cfgTransportBufSet(config, t, CFG_BUFFER_FULLY);
    assert_int_equal(cfgTransportBuf(config, t), CFG_BUFFER_FULLY);

    // Don't crash
    cfgTransportBufSet(NULL, t, CFG_BUFFER_FULLY);
    cfgTransportBuf(NULL, t);
    cfgTransportBufSet(config, t, CFG_BUFFER_LINE+1);

    cfgDestroy(&config);
}


static void
cfgCustomTagsSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    assert_null(cfgCustomTags(config));
    int i;
    for (i=0; i<10; i++) {
        char name[64];
        char value[64];
        snprintf(name, sizeof(name), "name%d", i);
        snprintf(value, sizeof(value), "value%d", i);

        assert_false(cfgCustomTagValue(config, name));
        cfgCustomTagAdd(config, name, value);
        assert_string_equal(cfgCustomTagValue(config, name), value);
        custom_tag_t** tags = cfgCustomTags(config);
        assert_non_null(tags[i]);
        assert_string_equal(tags[i]->name, name);
        assert_string_equal(tags[i]->value, value);
        assert_null(tags[i+1]);
    }

    // test that a tag can be overridden by a later tag
    cfgCustomTagAdd(config, "name0", "some other value");
    assert_string_equal(cfgCustomTagValue(config, "name0"), "some other value");

    // test that invalid values don't crash
    cfgCustomTagAdd(config, NULL, "something");
    cfgCustomTagAdd(config, "something", NULL);
    cfgCustomTagAdd(config, NULL, NULL);
    cfgCustomTagAdd(NULL, "something", "something else");

    cfgDestroy(&config);
}

static void
cfgLoggingSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgLogLevelSet(config, CFG_LOG_DEBUG);
    assert_int_equal(cfgLogLevel(config), CFG_LOG_DEBUG);
    cfgDestroy(&config);
}

static void
cfgLogLevelSetAndGet(void** state)
{
    config_t* config = cfgCreateDefault();
    cfgLogLevelSet(config, CFG_LOG_DEBUG);
    assert_int_equal(cfgLogLevel(config), CFG_LOG_DEBUG);
    cfgLogLevelSet(config, CFG_LOG_INFO);
    assert_int_equal(cfgLogLevel(config), CFG_LOG_INFO);
    cfgLogLevelSet(config, CFG_LOG_WARN);
    assert_int_equal(cfgLogLevel(config), CFG_LOG_WARN);
    cfgLogLevelSet(config, CFG_LOG_ERROR);
    assert_int_equal(cfgLogLevel(config), CFG_LOG_ERROR);
    cfgLogLevelSet(config, CFG_LOG_NONE);
    assert_int_equal(cfgLogLevel(config), CFG_LOG_NONE);
    cfgDestroy(&config);
}


int
main(int argc, char* argv[])
{
    printf("running %s\n", argv[0]);
    void* out_state[] = {(void*)CFG_OUT, NULL};
    void* evt_state[] = {(void*)CFG_EVT, NULL};
    void* log_state[] = {(void*)CFG_LOG, NULL};

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(cfgCreateDefaultReturnsValidPtr),
        cmocka_unit_test(accessorValuesForDefaultConfigAreAsExpected),
        cmocka_unit_test(accessorsReturnDefaultsWhenConfigIsNull),
        cmocka_unit_test(cfgOutFormatSetAndGet),
        cmocka_unit_test(cfgOutStatsDPrefixSetAndGet),
        cmocka_unit_test(cfgOutStatsDMaxLenSetAndGet),
        cmocka_unit_test(cfgOutVerbositySetAndGet),
        cmocka_unit_test(cfgOutPeriodSetAndGet),
        cmocka_unit_test(cfgCmdDirSetAndGet),
        cmocka_unit_test(cfgEventFormatSetAndGet),
        cmocka_unit_test(cfgEventLogFileFilterSetAndGet),
        cmocka_unit_test(cfgEventSourceSetAndGet),

        cmocka_unit_test_prestate(cfgTransportTypeSetAndGet, out_state),
        cmocka_unit_test_prestate(cfgTransportHostSetAndGet, out_state),
        cmocka_unit_test_prestate(cfgTransportPortSetAndGet, out_state),
        cmocka_unit_test_prestate(cfgTransportPathSetAndGet, out_state),
        cmocka_unit_test_prestate(cfgTransportBufSetAndGet,  out_state),

        cmocka_unit_test_prestate(cfgTransportTypeSetAndGet, evt_state),
        cmocka_unit_test_prestate(cfgTransportHostSetAndGet, evt_state),
        cmocka_unit_test_prestate(cfgTransportPortSetAndGet, evt_state),
        cmocka_unit_test_prestate(cfgTransportPathSetAndGet, evt_state),
        cmocka_unit_test_prestate(cfgTransportBufSetAndGet,  evt_state),

        cmocka_unit_test_prestate(cfgTransportTypeSetAndGet, log_state),
        cmocka_unit_test_prestate(cfgTransportHostSetAndGet, log_state),
        cmocka_unit_test_prestate(cfgTransportPortSetAndGet, log_state),
        cmocka_unit_test_prestate(cfgTransportPathSetAndGet, log_state),
        cmocka_unit_test_prestate(cfgTransportBufSetAndGet,  log_state),

        cmocka_unit_test(cfgCustomTagsSetAndGet),
        cmocka_unit_test(cfgLoggingSetAndGet),
        cmocka_unit_test(cfgLogLevelSetAndGet),
        cmocka_unit_test(dbgHasNoUnexpectedFailures),
    };
    return cmocka_run_group_tests(tests, groupSetup, groupTeardown);
}


