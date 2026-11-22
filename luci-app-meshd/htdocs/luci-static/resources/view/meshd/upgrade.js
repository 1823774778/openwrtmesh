'use strict';
'require view';
'require meshd';

// SPDX-License-Identifier: GPL-2.0-or-later

return view.extend({
	load() {
		return meshd.read('upgrade_status');
	},

	render(upgrade) {
		return E('div', [
			E('h2', [ _('Mesh Upgrade') ]),
			E('p', [ _('Managed node firmware upgrades are deferred. This page neither starts upgrades nor monitors per-device upgrade progress.') ]),
			upgrade.value ? E('p', [ meshd.flag(upgrade.value.available) === false
				? _('Mesh upgrade status is not available in V1.')
				: _('This page does not display upgrade telemetry.')
			]) : meshd.readError(upgrade)
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
