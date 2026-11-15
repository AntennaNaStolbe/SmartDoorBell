import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import {fromZigbee as fz, toZigbee as tz} from 'zigbee-herdsman-converters';

const e = exposes.presets;
const ea = exposes.access;

export default {
  zigbeeModel: ['SmartDoorBell'],
  model: 'SmartDoorBell',
  vendor: 'AntennaNS',
  description: 'ESP32-C6 smart doorbell with touch button, LED ring and Zigbee router',
  fromZigbee: [
    fz.on_off,
    fz.brightness,
    fz.color_colortemp,
    {
      cluster: 'genOnOff',
      type: 'commandOn',
      convert: () => ({action: 'single', last_press: new Date().toISOString()}),
    },
  ],
  toZigbee: [
    tz.light_onoff_brightness,
    tz.light_color,
    {
      key: ['ring'],
      convertSet: async (entity, key, value, meta) => {
        await entity.command('genIdentify', 'identify', {identifytime: 1}, {disableDefaultResponse: true});
        return {state: {[key]: value, last_press: new Date().toISOString()}};
      },
    },
  ],
  exposes: [
    e.light_brightness_colorxy(),
    e.enum('ring', ea.SET, ['ring'])
      .withDescription('Ring the doorbell')
      .withHomeAssistant({icon: 'mdi:bell-ring'}),
    e.text('last_press', ea.STATE)
      .withDescription('Last physical button press or remote ring')
      .withHomeAssistant({deviceClass: 'timestamp'}),
    e.action(['single']),
  ],
  ota: true,
};
