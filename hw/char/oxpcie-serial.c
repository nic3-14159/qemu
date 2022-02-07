/*
 * QEMU 16550A multi UART emulation
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2008 Citrix Systems, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* see docs/specs/pci-serial.rst */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"

typedef struct PCIOxpcieSerialState {
    PCIDevice    dev;
    MemoryRegion mmiobar;
    uint32_t     ports;
    char         *name[2];
    SerialMM     serial[2];
    uint32_t     level[2];
    qemu_irq     *irqs;
    uint8_t      prog_if;
} PCIOxpcieSerialState;

static void multi_serial_pci_exit(PCIDevice *dev)
{
    PCIOxpcieSerialState *pci = DO_UPCAST(PCIOxpcieSerialState, dev, dev);
    SerialState *s;
    int i;

    for (i = 0; i < pci->ports; i++) {
        s = &(pci->serial[i].serial);
        qdev_unrealize(DEVICE(s));
        memory_region_del_subregion(&pci->mmiobar, &s->io);
        g_free(pci->name[i]);
    }
    qemu_free_irqs(pci->irqs, pci->ports);
}

static void multi_serial_irq_mux(void *opaque, int n, int level)
{
    PCIOxpcieSerialState *pci = opaque;
    int i, pending = 0;

    pci->level[n] = level;
    for (i = 0; i < pci->ports; i++) {
        if (pci->level[i]) {
            pending = 1;
        }
    }
    pci_set_irq(&pci->dev, pending);
}

static void multi_serial_pci_realize(PCIDevice *dev, Error **errp)
{
    PCIOxpcieSerialState *pci = DO_UPCAST(PCIOxpcieSerialState, dev, dev);
    SerialState *s;
    size_t i, nports = 2;

    pci->dev.config[PCI_CLASS_PROG] = pci->prog_if;
    pci->dev.config[PCI_INTERRUPT_PIN] = 0x01;
    memory_region_init(&pci->mmiobar, OBJECT(pci), "multiserial", 16384);
    pci_register_bar(&pci->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &pci->mmiobar);
    pci->irqs = qemu_allocate_irqs(multi_serial_irq_mux, pci, nports);

    for (i = 0; i < nports; i++) {
        s = &(pci->serial[i].serial);
        if (!qdev_realize(DEVICE(s), NULL, errp)) {
            multi_serial_pci_exit(dev);
            return;
        }
        s->irq = pci->irqs[i];
        pci->name[i] = g_strdup_printf("uart #%zu", i + 1);
        memory_region_init_io(&s->io, OBJECT(pci), &serial_io_ops, s,
                              pci->name[i], 8);
        memory_region_add_subregion(&pci->mmiobar, 0x1000 + 0x200 * i, &s->io);
        pci->ports++;
    }
}

static const VMStateDescription vmstate_pci_multi_serial = {
    .name = "pci-oxpcie-serial",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PCIOxpcieSerialState),
        VMSTATE_STRUCT_ARRAY(serial, PCIOxpcieSerialState, 2,
                             0, vmstate_serial, SerialMM),
        VMSTATE_UINT32_ARRAY(level, PCIOxpcieSerialState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static Property multi_2x_serial_pci_properties[] = {
    DEFINE_PROP_CHR("chardev1",  PCIOxpcieSerialState, serial[0].serial.chr),
    DEFINE_PROP_CHR("chardev2",  PCIOxpcieSerialState, serial[1].serial.chr),
    DEFINE_PROP_UINT8("prog_if",  PCIOxpcieSerialState, prog_if, 0x02),
    DEFINE_PROP_END_OF_LIST(),
};

static void multi_2x_serial_pci_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = multi_serial_pci_realize;
    pc->exit = multi_serial_pci_exit;
    pc->vendor_id = PCI_VENDOR_ID_OXFORD;
    pc->device_id = PCI_DEVICE_ID_OXFORD_SERIAL;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_COMMUNICATION_SERIAL;
    dc->vmsd = &vmstate_pci_multi_serial;
    device_class_set_props(dc, multi_2x_serial_pci_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static void multi_serial_init(Object *o)
{
    PCIDevice *dev = PCI_DEVICE(o);
    PCIOxpcieSerialState *pms = DO_UPCAST(PCIOxpcieSerialState, dev, dev);
    size_t i, nports = 2; 

    for (i = 0; i < nports; i++) {
        object_initialize_child(o, "serial[*]", &pms->serial[i].serial, TYPE_SERIAL);
    }
}

static const TypeInfo multi_2x_serial_pci_info = {
    .name          = "oxpcie-serial",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIOxpcieSerialState),
    .instance_init = multi_serial_init,
    .class_init    = multi_2x_serial_pci_class_initfn,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};


static void multi_serial_pci_register_types(void)
{
    type_register_static(&multi_2x_serial_pci_info);
}

type_init(multi_serial_pci_register_types)
