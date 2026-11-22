// SPDX-License-Identifier: GPL-2.0-or-later
// Run with Node.js 18+: node luci-app-meshd/tests/smoke.cjs
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const resources = path.join(root, 'htdocs/luci-static/resources');
const json = file => JSON.parse(fs.readFileSync(path.join(root, file), 'utf8'));
const menu = json('root/usr/share/luci/menu.d/luci-app-meshd.json');
const acl = json('root/usr/share/rpcd/acl.d/luci-app-meshd.json');
const group = 'luci-app-meshd';
const parent = 'admin/network/mesh';
const reads = [ 'status', 'topology', 'link_quality', 'client_list', 'roam_config', 'config_status', 'diagnose', 'upgrade_status', 'event_log' ];
const writes = [ 'enroll', 'remove_node', 'config_apply', 'config_rollback' ];
assert.deepEqual(menu[parent].depends.acl, [ group ]);
assert.equal(menu[parent].action.type, 'firstchild');
assert.deepEqual(Object.keys(acl), [ group ]);
assert.deepEqual(acl[group].read, { ubus: { meshd: reads } });
assert.deepEqual(acl[group].write, { ubus: { meshd: writes } });
const pages = [ 'overview', 'nodes', 'roaming', 'policies', 'upgrade', 'diagnostics' ];
assert.deepEqual(Object.keys(menu).sort(), [ parent, ...pages.map(name => `${parent}/${name}`) ].sort());
for (const name of pages)
	assert.deepEqual(menu[`${parent}/${name}`].action, { type: 'view', path: `meshd/${name}` });

// LuCI DOM.append treats scalar content as HTML and array values as text.
class Element {
	constructor(tag, attrs) {
		this.tag = tag;
		this.attrs = attrs;
		this.id = attrs.id;
		this.children = [];
		this.events = {};
		this.value = '';
		this.checked = false;
		this.disabled = false;
	}
	addEventListener(event, fn) { this.events[event] = fn; }
	querySelectorAll(tag) {
		return this.children.flatMap(el => el instanceof Element
			? [ ...(el.tag === tag ? [ el ] : []), ...el.querySelectorAll(tag) ] : []);
	}
	prepend(el) { this.children.unshift(el); }
	focus() {}
	get textContent() { return this.children.map(el => el instanceof Element ? el.textContent : el).join(''); }
	set textContent(value) { this.children = [ String(value) ]; }
}
function content(node, data) {
	if (data === undefined)
		return;
	if (Array.isArray(data))
		node.children = data.map(el => el instanceof Element ? el : String(el));
	else if (data instanceof Element)
		node.children = [ data ];
	else
		assert.fail(`Scalar HTML sink: ${data}`);
}
function E(tag, attrs, data) {
	assert.match(tag, /^[a-z][a-z0-9]*$/);
	if (Array.isArray(attrs) || attrs instanceof Element) {
		data = attrs;
		attrs = {};
	}
	const el = new Element(tag, attrs ?? {});
	for (const [ event, fn ] of Object.entries(el.attrs)) {
		if (typeof fn === 'function')
			el.addEventListener(event, fn);
	}
	content(el, data);
	return el;
}

let writable = true, confirm = true;
const calls = [], declared = [];
const markup = '<img src=x onerror=alert(1)>';
const id = 'a'.repeat(64);
const replies = {
	status: { role: 'gateway', enabled: 1, transport: 0, node_id: id, backhaul: 'wds', applied_version: 1, error: markup },
	topology: { nodes: [ { id, role: 'candidate', ip: markup, model: markup, state: 'candidate', approved: 0, version: 0 } ] },
	link_quality: { available: 0 },
	client_list: { available: 0, clients: [] },
	upgrade_status: { available: false },
	roam_config: { available: 0 },
	config_status: { state: 'idle', version: 1 },
	diagnose: { checks: [{ name: markup, status: 'PASS', detail: markup }] },
	event_log: { events: [{ time: 1700000000, type: markup, detail: markup }] }
};
const scope = {
	E, _: text => text,
	L: { hasViewPermission: () => writable, url: p => `/${p}` },
	window: { confirm: () => confirm },
	baseclass: { extend: object => object }, view: { extend: object => object }, dom: { content },
	rpc: { declare({ object, method, params = [], reject }) {
		assert.equal(object, 'meshd');
		assert.equal(reject, true);
		declared.push(method);
		return async (...args) => {
			calls.push([ method, Object.fromEntries(params.map((name, i) => [ name, args[i] ])) ]);
			if (replies[method] instanceof Error)
				throw replies[method];
			return replies[method] ?? 0;
		};
	} }
};
const evaluate = p => new Function(...Object.keys(scope), fs.readFileSync(path.join(resources, p), 'utf8'))(...Object.values(scope));
scope.meshd = evaluate('meshd.js');
assert.deepEqual(declared, [ ...reads, ...writes ]);
const views = Object.fromEntries(pages.map(name => [ name, evaluate(`view/meshd/${name}.js`) ]));
const render = async name => views[name].render(await views[name].load());
const button = (page, text) => page.querySelectorAll('button').find(el => el.textContent.startsWith(text));
const input = (page, id) => page.querySelectorAll('input').find(el => el.id === `meshd-${id}`);
const mutations = () => calls.filter(([ name ]) => writes.includes(name));

async function main() {
	for (const view of Object.values(views))
		for (const handler of [ 'handleSave', 'handleSaveApply', 'handleReset' ])
			assert.equal(view[handler], null);
	assert.match((await render('overview')).textContent, /Enabled.*Unavailable.*Single-hop WDS.*Link quality measurements are not available/);
	for (const name of [ 'policies', 'upgrade' ])
		assert.equal((await render(name)).querySelectorAll('button').length, 0);
	assert.ok((await render('diagnostics')).textContent.includes(markup));
	replies.event_log = new Error(markup);
	const diagnosis = (await render('diagnostics')).textContent;
	assert.match(diagnosis, /Passed/);
	assert.match(diagnosis, /Unable to read Mesh data/);

	writable = false;
	const readonly = await render('nodes');
	assert.equal(readonly.querySelectorAll('button').length, 1);
	assert.ok(readonly.textContent.includes(markup));
	assert.equal((await render('roaming')).querySelectorAll('input').length, 0);
	writable = true;
	replies.status.role = 'node';
	assert.equal((await render('nodes')).querySelectorAll('button').length, 1);
	assert.equal((await render('roaming')).querySelectorAll('input').length, 0);
	replies.status.role = 'gateway';
	assert.equal((await render('nodes')).querySelectorAll('button').length, 1);
	assert.equal((await render('roaming')).querySelectorAll('input').length, 0);
	replies.status.transport = 1;
	const nodes = await render('nodes');
	const fingerprint = input(nodes, 'node-id');
	const authorize = nodes.querySelectorAll('button').findLast(el => el.textContent.startsWith('Authorize'));
	fingerprint.value = 'invalid';
	await authorize.events.click();
	assert.match(nodes.textContent, /complete 64-character/);
	fingerprint.value = `  ${'B'.repeat(64)}  `;
	confirm = false;
	await authorize.events.click();
	assert.deepEqual(mutations(), []);
	confirm = true;
	await button(nodes, 'Refresh').events.click();
	assert.equal(fingerprint.value, `  ${'B'.repeat(64)}  `);
	await authorize.events.click();
	assert.deepEqual(mutations().at(-1), [ 'enroll', { node_id: 'b'.repeat(64) } ]);
	fingerprint.value = id;
	await button(nodes, 'Remove').events.click();
	assert.deepEqual(mutations().at(-1), [ 'remove_node', { node_id: id } ]);

	const roam = await render('roaming');
	const ssid = input(roam, 'ssid'), key = input(roam, 'key'), beacon = input(roam, 'k_beacon');
	assert.equal(key.attrs.type, 'password');
	assert.equal(key.attrs.value, undefined);
	assert.equal(button(roam, 'Roll back'), undefined);
	ssid.value = '界'.repeat(11);
	ssid.events.input();
	await button(roam, 'Apply').events.click();
	assert.match(roam.textContent, /1–32 UTF-8 bytes/);
	ssid.value = 'test SSID';
	key.value = 'test key 1';
	beacon.checked = true;
	replies.roam_config = { available: 1, ssid: markup, k_beacon: 0, key: 'backend key' };
	await button(roam, 'Refresh status').events.click();
	assert.equal(ssid.value, 'test SSID');
	assert.equal(key.value, 'test key 1');
	assert.equal(beacon.checked, true);
	assert.ok(!roam.textContent.includes('backend key'));
	replies.roam_config.available = 0;
	await button(roam, 'Refresh status').events.click();
	await button(roam, 'Apply').events.click();
	assert.deepEqual(mutations().at(-1), [ 'config_apply', {
		ssid: 'test SSID', key: 'test key 1', k_neighbor: false, k_beacon: true, v_bss_transition: false, ft: false
	} ]);
	assert.equal(key.value, '');
	assert.match(roam.textContent, /Apply request accepted/);
	replies.config_status = { state: 'verifying', version: 1, target_version: 2 };
	await button(roam, 'Refresh status').events.click();
	assert.equal(roam.querySelectorAll('fieldset')[0].disabled, true);
	await button(roam, 'Roll back').events.click();
	assert.deepEqual(mutations().at(-1), [ 'config_rollback', {} ]);
	replies.config_status.state = 'committed';
	await button(roam, 'Refresh status').events.click();
	assert.equal(button(roam, 'Roll back'), undefined);
	assert.match(roam.textContent, /Committed locally/);
	assert.match(roam.textContent, /not a network-wide acknowledgment/);

	console.log('PASS: 6 LuCI pages, 9 read + 4 write ACL entries, text rendering, gateway write guards and local transaction flow');
}

main().catch(error => {
	console.error(error);
	process.exitCode = 1;
});
