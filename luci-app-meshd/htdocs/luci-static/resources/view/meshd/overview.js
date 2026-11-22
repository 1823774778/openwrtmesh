'use strict';
'require view';
'require meshd';

// SPDX-License-Identifier: GPL-2.0-or-later

function backhaulName(mode) {
	switch (mode) {
	case 'wired': return _('Wired');
	case 'wds': return _('Single-hop WDS');
	case 'manual': return _('Manually configured');
	default: return meshd.text(mode);
	}
}

return view.extend({
	load() {
		return Promise.all([ meshd.read('status'), meshd.read('link_quality') ]);
	},

	render([ status, quality ]) {
		const s = status.value;
		return E('div', [
			E('h2', [ _('Mesh Overview') ]),
			s ? meshd.facts([
				[ _('Mesh service'), meshd.flagText(s.enabled, _('Enabled'), _('Disabled')) ],
				[ _('Local role'), meshd.role(s.role) ],
				[ _('Local node fingerprint'), meshd.text(s.node_id) ],
				[ _('Control transport'), meshd.flagText(s.transport, _('Available'), _('Unavailable')) ],
				...(s.role === 'node' && s.gateway_connected != null ? [ [
					_('Gateway control contact'), meshd.flagText(s.gateway_connected, _('Recent'), _('Not recent'))
				] ] : []),
				[ _('Local applied configuration version'), meshd.text(s.applied_version) ],
				[ _('Configured backhaul'), backhaulName(s.backhaul) ],
				[ _('Last error'), meshd.text(s.error) ]
			]) : meshd.readError(status),
			E('h3', [ _('Backhaul and Link Quality') ]),
			E('p', [ _('V1 uses user-configured wired connections or single-hop WDS. Set up the backhaul before enabling Mesh; LuCI does not change the physical network or its wireless bridging configuration here.') ]),
			quality.value ? E('p', [ meshd.flag(quality.value.available) === false
				? _('Link quality measurements are not available in V1.')
				: _('This page does not display measured link quality.')
			]) : meshd.readError(quality)
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
