system_ss.add(when: 'CONFIG_REMOTE_PORT', if_true: files(
  'remote-port-proto.c',
  'remote-port.c',
  'remote-port-memory-master.c',
  'remote-port-memory-slave.c',
  'remote-port-gpio.c',
  'remote-port-stream.c',
  'remote-port-net.c',
  'remote-port-ats.c',
  'remote-port-qdev.c'))

system_ss.add(when: 'CONFIG_REMOTE_PORT_PCI', if_true: files(
  'remote-port-pci-adaptor.c',
  'remote-port-pci-device.c',
  'remote-port-pcie-root-port.c'))
