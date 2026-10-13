// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the portal's own script against a stub DOM and checks that it builds
// and wires up what it should.
//
// It exists because of a bug it would have caught: the call that builds the
// routing grid was never added, so the section rendered empty with only its
// help text, while the markup, the function and the styles were all present
// and the script parsed cleanly. Nothing short of opening the page in a
// browser would have noticed. A second one went the same way: a button could
// be chosen as the condition, but the page offered no way to say which
// button, and left the choice of trigger on screen where it did nothing.
const nodes = {};
function node(id) {
  return {
    id, innerHTML: '', textContent: '', value: '', checked: false,
    className: '', dataset: {}, type: '', style: {},
    listeners: {},
    addEventListener(kind, fn) {
      (this.listeners[kind] = this.listeners[kind] || []).push(fn);
    },
    // Lets a test press something, which is the only way to tell a control
    // that is merely drawn from one that is connected.
    fire(kind) { (this.listeners[kind] || []).forEach(fn => fn.call(this)); },
    select() {}, getBoundingClientRect: () => ({width: 320}),
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

// What the service would answer, and what the page asked for. The page draws
// itself from the reply, so steering the reply is how each condition is put
// on screen.
let state = { status: 'idle', path: '/x', config: {}, live: null };
const sent = [];
global.fetch = (url) => {
  sent.push(url);
  return Promise.resolve({ json: () => Promise.resolve(state) });
};
global.setInterval = () => 0;
// Timers the test can run on demand, so a four second arm does not mean a
// four second test.
const timers = [];
global.setTimeout = (fn) => { timers.push(fn); return timers.length; };
global.clearTimeout = (id) => { if (id) timers[id - 1] = null; };
const runTimers = () => {
  const due = timers.slice();
  timers.length = 0;
  due.forEach(fn => fn && fn());
};
const settle = () => new Promise(r => setImmediate(r));
// The page redraws itself from the service's reply, and in here nothing is
// polling, so a reply has to be asked for. Reload does that.
async function refresh() {
  nodes['reload'].fire('click');
  for (let i = 0; i < 8; i++) await settle();
}

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

let failures = 0;
function check(what, ok, got) {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what.padEnd(46)} ${got}`);
  if (!ok) failures++;
}

const grid = nodes['routing'].innerHTML;
const cells = (grid.match(/<button class='rcell' id='route_/g) || []).length;
const heads = (grid.match(/class='hd'/g) || []).length;
const rows  = (grid.match(/class='rw'/g) || []).length;
const modes = (grid.match(/<select class='mode' id='read_/g) || []).length;
const ids = [...grid.matchAll(/id='(route_[a-z_]+)'/g)].map(m => m[1]);
const want = ['yaw', 'pitch', 'roll']
  .flatMap(s => ['rx', 'ry', 'lx', 'ly'].map(d => `route_${s}_${d}`));
const missing = want.filter(id => !ids.includes(id));
if (missing.length) console.log(`  MISSING: ${missing.join(' ')}`);
check('the routing grid is built', cells === 12 && heads === 5 &&
      rows === 3 && modes === 3 && missing.length === 0,
      `${cells} cells, ${heads} heads, ${rows} rows, ${modes} modes`);

const chips = (nodes['btngrid'].innerHTML.match(/class='bchip' id='btn_/g) || []).length;
check('a chip is offered for every button', chips === 16, chips);

// Where it sits matters as much as whether it exists. Built as a section of
// its own it landed well below the condition that calls for it, so choosing a
// button meant scrolling off to find a part of the page you had to know was
// there. It belongs in the same group, in the slot the choice of trigger
// occupies, so switching the condition swaps one control for the other.
const form = nodes['form'].innerHTML;
const at = (id) => form.indexOf(`id='row_${id}'`);
const pos = { activation: at('activation'), mask: at('activation_mask'),
              trig: at('trigger_right'), thr: at('trigger_threshold') };
check('the buttons are built into the form, not a section apart',
      pos.mask > 0, pos.mask > 0 ? 'in the form' : 'not in the form');
check('the buttons sit with the condition that calls for them',
      pos.activation > 0 && pos.mask > pos.activation && pos.mask < pos.trig,
      `condition ${pos.activation}, buttons ${pos.mask}, trigger ${pos.trig}`);
// Nothing may come between them, or it is a different group.
const between = form.slice(pos.activation, pos.mask);
check('nothing is wedged between the condition and the buttons',
      (between.match(/id='row_/g) || []).length === 1,
      (between.match(/id='row_/g) || []).length - 1 + ' in between');

async function conditions() {
  state.config = { activation: 2, trigger_right: 0, trigger_threshold: 64,
                   activation_mask: 0x100 };
  await refresh();
  check('under a trigger, the choice of trigger is shown',
        nodes['row_trigger_right'].hidden === false,
        String(nodes['row_trigger_right'].hidden));
  check('under a trigger, the buttons are put away',
        nodes['row_activation_mask'].hidden === true,
        String(nodes['row_activation_mask'].hidden));

  state.config = { activation: 1, trigger_right: 0, trigger_threshold: 64,
                   activation_mask: 0x400 };
  await refresh();
  check('under a button, the buttons are shown',
        nodes['row_activation_mask'].hidden === false,
        String(nodes['row_activation_mask'].hidden));
  check('under a button, the trigger settings are put away',
        nodes['row_trigger_right'].hidden === true &&
        nodes['row_trigger_threshold'].hidden === true,
        String(nodes['row_trigger_right'].hidden));
  check('the chosen button is lit', / on$/.test(nodes['btn_4'].className),
        nodes['btn_4'].className || '(none)');

  // Pressing a chip has to reach the service, or the choice is decoration.
  sent.length = 0;
  nodes['btn_0'].fire('click');
  for (let i = 0; i < 8; i++) await settle();
  const asked = sent.filter(u => /activation_mask=/.test(u)).pop() || '';
  const got = /activation_mask=(\d+)/.exec(asked);
  check('pressing a chip sends the new mask',
        !!got && (Number(got[1]) & 0x4000) !== 0,
        got ? got[1] : 'nothing was sent');

  // Clearing the last one would leave a condition that can never be met.
  state.config = { activation: 1, activation_mask: 0x400 };
  await refresh();
  sent.length = 0;
  nodes['btn_4'].fire('click');
  await settle();
  check('the last button left cannot be cleared',
        !sent.some(u => /activation_mask=0(&|$)/.test(u)),
        sent.length ? 'a mask was sent' : 'nothing was sent');

  // Resetting throws away tuning that cannot be got back, so one press must
  // not do it. The button arms, and the arm has to lapse on its own, or a
  // stray press would sit waiting to be completed by an unrelated one later.
  sent.length = 0;
  nodes['defaults'].fire('click');
  await settle();
  check('one press does not reset anything',
        !sent.some(u => /api\/defaults/.test(u)),
        nodes['defaults'].textContent);
  check('the button says it wants another press',
        /again/i.test(nodes['defaults'].textContent) &&
        / armed$/.test(nodes['defaults'].className),
        nodes['defaults'].textContent);

  nodes['defaults'].fire('click');
  await settle();
  check('the second press resets', sent.some(u => /api\/defaults/.test(u)),
        sent.filter(u => /defaults/.test(u)).length + ' asked');
  check('and the button goes back to offering it',
        /^Reset/.test(nodes['defaults'].textContent),
        nodes['defaults'].textContent);

  sent.length = 0;
  nodes['defaults'].fire('click');     // arms
  runTimers();                         // and the arm lapses
  nodes['defaults'].fire('click');     // so this only arms again
  await settle();
  check('an arm left alone lapses rather than waiting',
        !sent.some(u => /api\/defaults/.test(u)),
        nodes['defaults'].textContent);

  console.log(failures ? '\npage checks FAILED' : '\nall page checks passed');
  process.exit(failures ? 1 : 0);
}
conditions();
