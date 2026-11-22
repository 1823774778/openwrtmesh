'use strict';
'require baseclass';
'require rpc';

// SPDX-License-Identifier: GPL-2.0-or-later

function declare(method, params) {
	return rpc.declare({ object: 'meshd', method, params, reject: true });
}

const reads = {
	status: declare('status'),
	topology: declare('topology'),
	link_quality: declare('link_quality'),
	client_list: declare('client_list'),
	roam_config: declare('roam_config'),
	config_status: declare('config_status'),
	diagnose: declare('diagnose'),
	upgrade_status: declare('upgrade_status'),
	event_log: declare('event_log')
};

return baseclass.extend({
	async read(method) {
		try {
			const value = await reads[method]();
			if (!value || typeof value !== 'object' || Array.isArray(value))
				throw new Error(_('Unexpected reply from the Mesh service.'));
			return { value, error: null };
		}
		catch (error) {
			return { value: null, error };
		}
	},

	enroll: declare('enroll', [ 'node_id' ]),
	removeNode: declare('remove_node', [ 'node_id' ]),
	applyConfig: declare('config_apply', [ 'ssid', 'key', 'k_neighbor', 'k_beacon', 'v_bss_transition', 'ft' ]),
	rollbackConfig: declare('config_rollback'),

	flag(value) {
		if (typeof value === 'boolean')
			return value;
		return Number.isFinite(value) ? value !== 0 : null;
	},

	flagText(value, yes, no) {
		switch (this.flag(value)) {
		case true: return yes;
		case false: return no;
		default: return _('Unknown');
		}
	},

	canManage(status) {
		return L.hasViewPermission() === true && status?.role === 'gateway' && this.flag(status.enabled) === true;
	},

	transportReady(status) {
		return this.flag(status?.transport) === true;
	},

	managementHint(status) {
		if (L.hasViewPermission() !== true)
			return _('This session has read-only Mesh access.');
		if (!status)
			return _('Mesh management is unavailable until the local status can be read.');
		if (!this.canManage(status))
			return _('These actions are available only on an enabled Mesh gateway.');
		return _('The gateway control transport is unavailable. Check Mesh Diagnostics before retrying.');
	},

	text(value) {
		return ((typeof value === 'string' && value.length > 0) || Number.isFinite(value)) ? String(value) : '—';
	},

	role(value) {
		switch (value) {
		case 'gateway': return _('Gateway');
		case 'node': return _('Node');
		case 'candidate': return _('Candidate');
		default: return this.text(value);
		}
	},

	button(label, handler, actionClass = 'neutral') {
		return E('button', {
			type: 'button',
			class: 'cbi-button cbi-button-' + actionClass,
			click: handler
		}, [ label ]);
	},

	readError(result) {
		return E('p', { class: 'alert-message warning' }, [
			_('Unable to read Mesh data: '), this.text(result.error?.message)
		]);
	},

	facts(rows) {
		// LuCI treats scalar children of E() as HTML; arrays create text nodes.
		return E('table', { class: 'table' }, E('tbody', rows.map(([ label, value ]) =>
			E('tr', { class: 'tr' }, [
				E('th', { class: 'th', scope: 'row' }, [ label ]),
				E('td', { class: 'td', style: 'overflow-wrap:anywhere' }, [ value ])
			])
		)));
	},

	table(headers, rows, empty) {
		if (!rows.length)
			return E('p', [ empty ]);

		return E('div', { style: 'overflow-x:auto' }, E('table', { class: 'table' }, [
			E('thead', E('tr', { class: 'tr table-titles' }, headers.map(label =>
				E('th', { class: 'th', scope: 'col' }, [ label ])
			))),
			E('tbody', rows.map(row => E('tr', { class: 'tr' }, row.map(value =>
				E('td', { class: 'td', style: 'overflow-wrap:anywhere' }, [ value ])
			))))
		]));
	},

	items(value) {
		return Array.isArray(value) ? value.filter(item => item && typeof item === 'object' && !Array.isArray(item)) : [];
	}
});
