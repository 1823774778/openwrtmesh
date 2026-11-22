'use strict';
'require view';
'require meshd';

// SPDX-License-Identifier: GPL-2.0-or-later

return view.extend({
	load() {
		return meshd.read('client_list');
	},

	render(clients) {
		return E('div', [
			E('h2', [ _('Mesh Policies') ]),
			E('p', [ _('Mesh-wide guest networks, QoS, and parental controls are deferred. This page does not configure those features or change any node settings.') ]),
			E('h3', [ _('Connected Clients') ]),
			clients.value ? E('p', [ meshd.flag(clients.value.available) === false
				? _('V1 does not collect a Mesh-wide client list. An empty result does not mean that no clients are connected.')
				: _('This page does not display client telemetry.')
			]) : meshd.readError(clients)
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
