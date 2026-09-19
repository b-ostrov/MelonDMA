/* mlx_mrrs — read or set the card's PCIe Max Read Request Size.
 *
 *   mlx_mrrs              print Device Control / Capabilities
 *   mlx_mrrs 4096         set MRRS (128..4096, power of two); kept across FLR
 *   mlx_mrrs restore      back to the value the driver found
 *
 * MRRS bounds how much the card asks the host for in one read request; the
 * host still answers in max-payload-sized completions (128 B on the
 * Thunderbolt path). Out-of-Mac traffic is card reads of host memory, so this
 * is the one software-side knob on that direction. Change it only with no
 * traffic in flight: it applies to every client of the device.
 * Needs the diagnostic entitlement (build/mlx_mrrs is signed with it). */
#include "MlxUCIO.h"
#include "MlxServiceMatch.h"
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct mlx_pcie_devctl_req req = { .op = MLX_PCIE_DEVCTL_READ };
    if (argc == 2 && !strcmp(argv[1], "restore")) {
        req.op = MLX_PCIE_DEVCTL_RESTORE;
    } else if (argc == 2) {
        req.op = MLX_PCIE_DEVCTL_SET_MRRS;
        req.mrrsBytes = (uint32_t)strtoul(argv[1], NULL, 0);
    } else if (argc != 1) {
        fprintf(stderr, "usage: mlx_mrrs [BYTES|restore]\n");
        return 2;
    }
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, mlxCreateServiceMatching());
    if (!service) { fprintf(stderr, "MelonDMA driver not found\n"); return 1; }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &c);
    IOObjectRelease(service);
    if (kr) { fprintf(stderr, "IOServiceOpen 0x%x\n", kr); return 1; }
    struct mlx_pcie_devctl_req out = {0};
    size_t outsize = sizeof(out);
    kr = IOConnectCallStructMethod(c, kMlxUCMethodPcieDevCtl, &req, sizeof(req), &out, &outsize);
    IOServiceClose(c);
    if (kr) {
        fprintf(stderr, "PcieDevCtl 0x%x%s\n", kr,
                kr == (kern_return_t)0xe00002c7 ? " (driver without this method: rebuild and reinstall it)" :
                kr == (kern_return_t)0xe00002e2 ? " (needs the diagnostic entitlement)" : "");
        return 1;
    }
    printf("devctl=0x%04x (at start 0x%04x) devcap=0x%08x mrrs=%u mps=%u mps_supported=%u override=%u\n",
           out.devCtl, out.devCtlAtStart, out.devCap, out.mrrsNow, out.mpsNow, out.mpsSupported,
           out.mrrsOverride);
    return 0;
}
