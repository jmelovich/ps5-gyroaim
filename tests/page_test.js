// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the portal's own script against a stub DOM and checks that it builds
// what it should.
//
// It exists because of a bug it would have caught: the call that builds the
// routing grid was never added, so the section rendered empty with only its
// help text, while the markup, the function and the styles were all present
// and the script parsed cleanly. Nothing short of opening the page in a
// browser would have noticed.
const nodes = {};
function node(id) {
  return {
    id, innerHTML: '', textContent: '', value: '', checked: false,
    className: '', dataset: {}, type: '', style: {},
    addEventListener() {}, select() {}, getBoundingClientRect: () => ({width: 320}),
    getContext: () => ({ setTransform(){}, clearRect(){}, strokeRect(){},
      beginPath(){}, moveTo(){}, lineTo(){}, stroke(){}, fill(){}, arc(){},
      fillRect(){}, fillText(){}, save(){}, restore(){}, translate(){},
      rotate(){}, setLineDash(){} }),
  };
}
global.document = {
  getElementById(id) { return nodes[id] || (nodes[id] = node(id)); },
  activeElement: null,
  body: {},
};
global.window = { devicePixelRatio: 1, addEventListener() {} };
global.getComputedStyle = () => ({ getPropertyValue: () => '#000' });
global.fetch = () => Promise.resolve({ json: () => Promise.resolve({}) });
global.setInterval = () => 0;

const fs = require('fs');
const path = require('path');
const page = fs.readFileSync(
  path.join(__dirname, '..', 'payload', 'web', 'portal.html'), 'utf8');
const script = /<script>([\s\S]*)<\/script>/.exec(page);
if (!script) {
  console.error('  no script found in the page');
  process.exit(1);
}
new Function(script[1])();

const grid = nodes['routing'].innerHTML;
const cells = (grid.match(/<button class='rcell' id='route_/g) || []).length;
const heads = (grid.match(/class='hd'/g) || []).length;
const rows  = (grid.match(/class='rw'/g) || []).length;
console.log(`  grid cells:   ${cells} (expect 12)`);
console.log(`  column heads: ${heads} (expect 5: one blank + four)`);
console.log(`  row labels:   ${rows} (expect 3)`);

// Every cell id the script will later try to address must exist.
const ids = [...grid.matchAll(/id='(route_[a-z_]+)'/g)].map(m => m[1]);
console.log(`  ids: ${ids.join(' ')}`);
const want = ['yaw', 'pitch', 'roll']
  .flatMap(s => ['rx', 'ry', 'lx', 'ly'].map(d => `route_${s}_${d}`));
const missing = want.filter(id => !ids.includes(id));
const ok = cells === 12 && heads === 5 && rows === 3 && missing.length === 0;
if (missing.length) console.log(`  MISSING: ${missing.join(' ')}`);
console.log(ok ? '\nall page checks passed' : '\npage checks FAILED');
process.exit(ok ? 0 : 1);
