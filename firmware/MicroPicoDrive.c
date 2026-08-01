#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "MicroDriveControl.h"
#include "UserInterface.h"

int main()
{
    //Start the MD control in core1
    multicore_launch_core1(RunUserInterface);

    //Opt this core into the flash lockout used by the SD firmware update
    //running on the other core (see sd_update.c)
    flash_safe_execute_core_init();

    //Run the user interface in core0
    RunMDControl();

    return 0;
}
