// Night Rider: what the controllers show and the CC each control sends (channel 1).
// src/app.cpp binds the same numbers in kBind.
const semis = v => `+${v} st`;
export const profile = {
  name: 'Night Rider',
  accent: '#c8322b',
  pages: [
    {key: 'P', label: 'PLAY', title: 'Ride', color: '#E04A3A', controls: [
      {id: 'whammy', cc: 30, kind: 'trigger', label: 'BEND', key: 'A'},
      {id: 'octave', cc: 31, kind: 'trigger', label: '+12', key: 'S'},
      {id: 'nitro', cc: 32, kind: 'trigger', label: 'NITRO', key: 'D'},
      {id: 'dryHold', cc: 33, kind: 'trigger', label: 'DRY', key: 'F'},
      {id: 'road', kind: 'xy', label: 'Throttle × Mix', x: 'throttle', y: 'mix'},
    ]},
    {key: 'R', label: 'RIDE', title: 'The bike', color: '#4FC9D4', controls: [
      {id: 'throttle', cc: 22, kind: 'range', label: 'Throttle', min: 0, max: 24, def: 0, format: v => v ? semis(v) : 'Rest'},
      {id: 'range', cc: 20, kind: 'range', label: 'Whammy range', min: 1, max: 24, def: 24, format: semis},
      {id: 'style', cc: 21, kind: 'choice', label: 'Style', options: ['CRUISE', 'SPORT', 'TURBO'], def: 1},
      {id: 'speed', cc: 26, kind: 'readout', label: 'Speed', min: 20, max: 300, def: 20, format: v => `${v} km/h`},
    ]},
    {key: 'M', label: 'MIX', title: 'Mix', color: '#F2B14C', controls: [
      {id: 'mix', cc: 24, kind: 'range', label: 'Mix', min: 0, max: 100, def: 100, unit: '%'},
      {id: 'dry', cc: 23, kind: 'toggle', label: 'Dry', def: 0},
      {id: 'level', cc: 25, kind: 'range', label: 'Level', min: 0, max: 100, def: 80, unit: '%'},
    ]},
  ],
};
