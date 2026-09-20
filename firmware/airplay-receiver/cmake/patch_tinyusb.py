"""Generate narrowly patched sources; leave managed_components untouched.
Pinned to espressif/tinyusb 0.21.0~2. Fail closed if upstream context changes.
"""
import pathlib
import sys
source, output = map(pathlib.Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
def patch(path, old, new):
    text = (source / path).read_text()
    if text.count(old) != 1:
        raise SystemExit(f'TinyUSB patch context mismatch: {path}')
    text = text.replace(old, new)
    (output / pathlib.Path(path).name).write_text(text)
patch('src/device/usbd.c', '''  // index is cfg_num-1
  const tusb_desc_configuration_t *desc_cfg =
    (const tusb_desc_configuration_t *)tud_descriptor_configuration_cb(cfg_num - 1);
''', '''  // USB configuration values are not descriptor indices (our only value is 2).
  const tusb_desc_device_t *device = (const tusb_desc_device_t *)tud_descriptor_device_cb();
  const tusb_desc_configuration_t *desc_cfg = NULL;
  for (uint8_t index = 0; index < device->bNumConfigurations; index++) {
    const tusb_desc_configuration_t *candidate =
      (const tusb_desc_configuration_t *)tud_descriptor_configuration_cb(index);
    if (candidate && candidate->bConfigurationValue == cfg_num) { desc_cfg = candidate; break; }
  }
''')
patch('src/portable/synopsys/dwc2/dcd_dwc2.c', '''  dcd_connect(rhport);
  return true;
''', '''  // Application deliberately connects only after its startup delay.
  return true;
''')
# Device has no I2S producer clock. Use a fractional USB-frame clock instead
# of TinyUSB's adaptive I2S FIFO flow controller. Always send a complete frame
# packet, filling FIFO starvation with silence and measuring it separately.
path = 'src/class/audio/audio_device.c'
text = (source/path).read_text()
a = text.index('  #if CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL\n', text.index('static bool audiod_tx_xfer_isr(uint8_t rhport, audiod_function_t * audio'))
b = text.index('  #if !CFG_TUD_EDPT_DEDICATED_HWFIFO', a)
text = text[:a] + '''  extern uint16_t ipod_usb_packet_bytes_isr(void);
  n_bytes_tx = tu_min16(ipod_usb_packet_bytes_isr(), audio->ep_in_sz);
''' + text[b:]
old = '  tu_fifo_read_n(&audio->ep_in_ff, audio->lin_buf_in, n_bytes_tx);'
assert text.count(old) == 1
text = text.replace(old, '''  uint16_t copied = tu_fifo_read_n(&audio->ep_in_ff, audio->lin_buf_in, n_bytes_tx);
  if (copied < n_bytes_tx) {
    memset(audio->lin_buf_in + copied, 0, n_bytes_tx - copied);
    extern void ipod_usb_fifo_starved_isr(uint16_t missing);
    ipod_usb_fifo_starved_isr(n_bytes_tx - copied);
  }''')
# A transfer error must not be reported as successful delivery.
old = 'audiod_tx_xfer_isr(rhport, audio, (uint16_t) xferred_bytes);'
assert text.count(old) == 1
text = text.replace(old, 'audiod_tx_xfer_isr(rhport, audio, result == XFER_RESULT_SUCCESS ? (uint16_t) xferred_bytes : 0);')
(output/'audio_device.c').write_text(text)
