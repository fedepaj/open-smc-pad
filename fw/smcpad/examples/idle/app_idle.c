#include "system/app_core.h"
#include "system/includes.h"
#include "server/server_core.h"
#include "app_config.h"
#include "app_action.h"

#define LOG_TAG_CONST       APP_IDLE
#define LOG_TAG             "[APP_IDLE]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"


#if CONFIG_APP_IDLE

#if TCFG_USER_EDR_ENABLE || TCFG_USER_BLE_ENABLE
//默认应用不需要打开配置,有需要自己添加
/* #error " confirm, need disable !!!!!!" */
#endif

extern void smcpad_bringup_start(void);
extern void smcpad_trace(const char *tag);

static void console_task(void *priv)
{
    smcpad_bringup_start();                 /* never returns */
}

static void idle_app_start()
{
    log_info("=======================================");
    log_info("---------idle demo---------");
    log_info("=======================================");
    log_info("app_file: %s", __FILE__);

    /* The bring-up console never returns, so it gets a task of its own ("smcpad_con" in
     * task_info_table). Run here, in app_core, it kept this task from ever taking its event
     * queue again: sys_event_task waits for each event it posts to be taken, and after its
     * timeout it calls chip_reset() -- the pad rebooted every ~40 s unless the console was
     * kept busy. No clk_set() here: the console and USB run on the clock the SDK set up. */
    smcpad_trace("idle: app started");
    task_create(console_task, NULL, "smcpad_con");
}

static int idle_event_handler(struct application *app, struct sys_event *event)
{
    switch (event->type) {
    case SYS_KEY_EVENT:
        return 0;
    case SYS_BT_EVENT:
        return 0;
    case SYS_DEVICE_EVENT:
        return 0;
    default:
        return false;
    }
}

static int idle_state_machine(struct application *app, enum app_state state,
                              struct intent *it)
{
    switch (state) {
    case APP_STA_CREATE:
        break;
    case APP_STA_START:
        if (!it) {
            break;
        }
        switch (it->action) {
        case ACTION_IDLE_MAIN:
            log_info("ACTION_IDLE_MAIN\n");
            /* os_taskq_flush(); */
            idle_app_start();
            break;
        }
        break;
    case APP_STA_PAUSE:
        break;
    case APP_STA_RESUME:
        break;
    case APP_STA_STOP:
        break;
    case APP_STA_DESTROY:
        break;
    }

    return 0;
}

static const struct application_operation app_idle_ops = {
    .state_machine  = idle_state_machine,
    .event_handler 	= idle_event_handler,
};

REGISTER_APPLICATION(app_app_idle) = {
    .name 	= "idle",
    .action	= ACTION_IDLE_MAIN,
    .ops 	= &app_idle_ops,
    .state  = APP_STA_DESTROY,
};

#endif

