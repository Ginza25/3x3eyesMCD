#include <stdio.h>
#include <stdint.h>

int main(void)
{
    uint16_t raw;
    printf("Enter 13-bit Event Flag ID (hex 0x#### or decimal): ");
    if (scanf("%hi", &raw) != 1) return 1;

    /* ---- mirror of the assembly ---- */
    uint16_t id = raw & 0x1FFF;          /* keep 13 bits                */

    uint32_t baseAddr;                   /* base of the byte selected   */
    uint32_t bankStartBit;               /* cumulative bit# of bank #0  */
    uint16_t local;                      /* flag number inside bank     */

    if (id < 0x0100)                     /* -------- $BE180 block ------ */
    {
        baseAddr      = 0xBE180;
        bankStartBit  = 1024;            /* comes after $BE100 block    */
        local         = id;
    }
    else if (id < 0x1100)                /* -------- $BE200 block ------ */
    {
        id           -= 0x0100;
        baseAddr      = 0xBE200;
        bankStartBit  = 2048;            /* $BE100 (1024) + $BE180(1024)*/
        local         = id & 0x0FFF;
    }
    else                                 /* -------- $BE100 block ------ */
    {
        id           -= 0x1200;
        baseAddr      = 0xBE100;
        bankStartBit  = 0;               /* first bank                  */
        local         = id & 0x03FF;
    }

    /* byte & bit inside the chosen bank */
    uint16_t byteOffset =  local >> 3;          /* /8  */
    uint8_t  bitOffset  = local & 7;            /* %8  */

    uint32_t byteAddr   = baseAddr + byteOffset;

    /* flat “unique bit” index where first bit of $BE100 == 0            */
    uint32_t uniqueID   = bankStartBit + (byteOffset << 3) + bitOffset;

    /* ---- output ---- */
    printf("\nResult\n======\n");
    printf("Flag bank   : $%05X\n", baseAddr);
    printf("Byte address: $%05X\n", byteAddr);
    printf("Bit inside  : %u\n",   bitOffset);
    printf("Unique flag#: %u\n",   uniqueID);
    return 0;
}