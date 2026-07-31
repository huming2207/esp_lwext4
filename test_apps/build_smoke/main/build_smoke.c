#include <ext4.h>
#include <ext4_debug.h>

void app_main(void)
{
    /*
     * Referencing code in ext4.c makes the final firmware link validate the
     * BSD xattr stubs as well as the external archive itself.
     */
    ext4_dmask_set(0);
    (void)ext4_device_unregister("unused");
}
