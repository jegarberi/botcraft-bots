// Offline DOM/API smoke check: node bot/test_web_spawn.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(__dirname + '/web/index.html', 'utf8');
assert.match(html, /id="spawn-count"[^>]*min="1"[^>]*max="100"[^>]*step="1"/);
const nodes = new Map(), requests = [];
let bots = [];
const element = () => ({ append() {}, replaceChildren() {}, addEventListener() {}, setAttribute() {} });
const document = {
    getElementById(id) { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); },
    createElement: element,
    addEventListener() {},
};
let contextCall;
vm.runInNewContext(html.match(/<script>([\s\S]*?)<\/script>/)[1] + '\nexposeCall(call);', {
    exposeCall(fn) { contextCall = fn; },
    document, setInterval() {}, setTimeout() {},
    fetch: async (url, options) => {
        if (options) requests.push(JSON.parse(options.body));
        return { json: async () => url === '/state' ? { bots } : { ok: true, result: ['Bot2', 'Bot3'] } };
    },
});
document.getElementById('spawn-count').value = '2';
document.getElementById('spawn-base').value = 'Worker';
assert.match(html, /label for="spawn-base"/);
assert.match(html, /id="spawn-base"[^>]*maxlength="15"[^>]*required/);
let prevented = false;
nodes.get('spawn').onsubmit({ preventDefault() { prevented = true; } });
assert(prevented);
assert.deepEqual(requests[0], { name: 'spawn', arguments: { count: 2, base_name: 'Worker' } });
setImmediate(async () => {
    assert.match(nodes.get('message').textContent, /spawn:/);
    bots = [{ name: 'Worker1' }, { name: 'Worker2' }];
    document.getElementById('all-bots').checked = true;
    // Exercise the same action handler used by each card.
    await contextCall('farm', { bot: 'Worker1' });
    assert.deepEqual(requests.slice(-2), [
        { name: 'farm', arguments: { bot: 'Worker1' } },
        { name: 'farm', arguments: { bot: 'Worker2' } },
    ]);
    assert.match(nodes.get('message').textContent, /Worker1:/);
    assert.match(nodes.get('message').textContent, /Worker2:/);
    document.getElementById('all-bots').checked = false;
    await contextCall('store', { bot: 'Worker2' });
    assert.deepEqual(requests.at(-1), { name: 'store', arguments: { bot: 'Worker2' } });
    const submitAction = () => nodes.get('bot-action').onsubmit({ preventDefault() {} });
    document.getElementById('all-bots').checked = true;
    document.getElementById('action-name').value = 'gather';
    document.getElementById('action-resource').value = 'iron_ore';
    document.getElementById('action-count').value = '12';
    await submitAction();
    assert.deepEqual(requests.slice(-2), bots.map(bot => ({ name: 'autopilot', arguments: { bot: bot.name, count: 12, gather: 'iron_ore', enabled: true } })));
    document.getElementById('all-bots').checked = false;
    document.getElementById('action-target').value = 'Worker2';
    document.getElementById('action-name').value = 'make';
    document.getElementById('action-resource').value = 'torch';
    await submitAction();
    assert.deepEqual(requests.at(-1), { name: 'make', arguments: { bot: 'Worker2', count: 12, item: 'torch' } });
    const beforeInvalid = requests.length;
    document.getElementById('action-count').value = '0';
    await submitAction();
    assert.equal(requests.length, beforeInvalid);
    document.getElementById('action-count').value = '12';
    document.getElementById('action-resource').value = '';
    await submitAction();
    assert.equal(requests.length, beforeInvalid);
    document.getElementById('action-resource-list').onchange({ target: { value: 'diamond_ore' } });
    assert.equal(nodes.get('action-resource').value, 'diamond_ore');
    document.getElementById('action-name').value = 'mine';
    await submitAction();
    assert.deepEqual(requests.at(-1), { name: 'autopilot', arguments: { bot: 'Worker2', count: 12, gather: 'diamond_ore', enabled: true } });
    document.getElementById('all-bots').checked = true;
    document.getElementById('action-name').value = 'castle-start';
    document.getElementById('action-position').value = '0 64 -10';
    const beforeCastle = requests.length;
    await submitAction();
    assert.equal(requests.length, beforeCastle + 1);
    assert.deepEqual(requests.at(-1), { name: 'castle', arguments: { action: 'start', x: 0, y: 64, z: -10 } });
    for (const action of ['roads', 'farms', 'ranch']) {
        nodes.get('action-name').value = 'castle-' + action;
        const before = requests.length;
        await submitAction();
        assert.equal(requests.length, before + 1);
        assert.deepEqual(requests.at(-1), { name: 'castle', arguments: { action } });
    }
    nodes.get('action-name').value = 'make_farm';
    await submitAction();
    assert.deepEqual(requests.slice(-2), bots.map(bot => ({ name: 'make_farm', arguments: { bot: bot.name, radius: 4 } })));
    nodes.get('action-name').value = 'shelter';
    await submitAction();
    assert.deepEqual(requests.slice(-2), bots.map(bot => ({ name: 'team_member', arguments: { bot: bot.name, objective: 'build', plan: 'shelter', item: 'cobblestone', x1: 0, y1: 64, z1: -10 } })));
    nodes.get('action-position').value = 'bad coords';
    const beforeBadCoordinates = requests.length;
    await submitAction();
    assert.equal(requests.length, beforeBadCoordinates);
    bots = [];
    document.getElementById('all-bots').checked = true;
    const before = requests.length;
    await contextCall('farm', { bot: 'Worker1' });
    assert.equal(requests.length, before);
    assert.equal(nodes.get('message').textContent, 'No bot connected.');
    console.log('web OK: spawn, all-bot actions, single-bot actions, action list, construction, resource list, invalid inputs, empty fleet');
});
