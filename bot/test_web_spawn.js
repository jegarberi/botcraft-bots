// Offline DOM/API smoke check: node bot/test_web_spawn.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(__dirname + '/web/index.html', 'utf8');
assert.match(html, /id="spawn-count"[^>]*min="1"[^>]*max="100"[^>]*step="1"/);
const nodes = new Map(), requests = [];
const element = () => ({ append() {}, replaceChildren() {}, addEventListener() {}, setAttribute() {} });
const document = {
    getElementById(id) { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); },
    createElement: element,
    addEventListener() {},
};
vm.runInNewContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], {
    document, setInterval() {}, setTimeout() {},
    fetch: async (url, options) => {
        if (options) requests.push(JSON.parse(options.body));
        return { json: async () => url === '/state' ? { bots: [] } : { ok: true, result: ['Bot2', 'Bot3'] } };
    },
});
document.getElementById('spawn-count').value = '2';
let prevented = false;
nodes.get('spawn').onsubmit({ preventDefault() { prevented = true; } });
assert(prevented);
assert.deepEqual(requests[0], { name: 'spawn', arguments: { count: 2 } });
setImmediate(() => {
    assert.match(nodes.get('message').textContent, /spawn:/);
    console.log('web spawn OK: labeled bounded count, submission, queued result');
});
