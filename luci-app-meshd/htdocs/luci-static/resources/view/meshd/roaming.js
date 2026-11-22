'use strict';
'require view';
'require dom';
'require meshd';

// SPDX-License-Identifier: GPL-2.0-or-later

const options = [
	[ 'k_neighbor', _('802.11k neighbor reports') ],
	[ 'k_beacon', _('802.11k beacon reports') ],
	[ 'v_bss_transition', _('802.11v BSS transition') ],
	[ 'ft', _('802.11r fast transition') ]
];

function configState(value) {
	switch (value) {
	case 'idle': return _('Idle');
	case 'applying': return _('Applying locally');
	case 'verifying': return _('Verifying locally');
	case 'committed': return _('Committed locally');
	case 'rolling_back': return _('Rolling back locally');
	case 'rolled_back': return _('Rolled back locally');
	case 'failed': return _('Failed');
	default: return meshd.text(value);
	}
}

function isPending(state) {
	return state === 'applying' || state === 'verifying';
}

function canStart(state) {
	return [ 'idle', 'committed', 'rolled_back', 'failed' ].includes(state);
}

function field(label, input) {
	return E('div', { class: 'cbi-value' }, [
		E('label', { class: 'cbi-value-title', for: input.id }, [ label ]),
		E('div', { class: 'cbi-value-field' }, [ input ])
	]);
}

return view.extend({
	load() {
		return Promise.all([ meshd.read('status'), meshd.read('roam_config'), meshd.read('config_status') ]);
	},

	render(initial) {
		let localStatus, reportedConfig, transaction, busy = false, edited = false;
		const summaries = E('div');
		const controls = E('div');
		const feedback = E('p', { role: 'status', 'aria-live': 'polite' });
		const ssid = E('input', {
			id: 'meshd-ssid', type: 'text', class: 'cbi-input-text', autocomplete: 'off',
			spellcheck: 'false', autocapitalize: 'none'
		});
		const key = E('input', {
			id: 'meshd-key', type: 'password', class: 'cbi-input-password', autocomplete: 'new-password',
			spellcheck: 'false', autocapitalize: 'none'
		});
		const flags = options.map(([ name ]) => E('input', { id: 'meshd-' + name, type: 'checkbox' }));
		[ ssid, key, ...flags ].forEach(input => input.addEventListener('input', () => { edited = true; }));

		const refresh = meshd.button(_('Refresh status'), () => run(async () => update(await this.load())));
		const apply = meshd.button(_('Apply wireless configuration…'), () => submit.call(this), 'apply');
		const rollback = meshd.button(_('Roll back pending local transaction…'), () => cancel.call(this), 'negative');
		const editor = E('fieldset', { class: 'cbi-section' }, [
			E('legend', [ _('Gateway Wireless Configuration') ]),
			E('p', [ _('Applying changes the preselected APs on this gateway and may disconnect wireless clients. After local verification, the gateway sends a signed version to nodes for asynchronous application.') ]),
			field(_('SSID'), ssid),
			field(_('New Wi-Fi key'), key),
			E('p', [ _('Enter 8–63 printable ASCII characters or 64 hexadecimal digits. The existing key is not shown.') ]),
			...options.map(([ , label ], index) => field(label, flags[index])),
			E('p', [ apply ])
		]);

		function syncButtons() {
			refresh.disabled = busy;
			editor.disabled = busy || !canStart(transaction?.state);
			rollback.disabled = busy || !isPending(transaction?.state);
		}

		async function run(action) {
			if (busy)
				return;
			busy = true;
			syncButtons();
			try {
				await action();
			}
			finally {
				busy = false;
				syncButtons();
			}
		}

		function update([ status, roam, config ]) {
			localStatus = status.value;
			reportedConfig = roam.value;
			transaction = config.value;
			const available = meshd.flag(reportedConfig?.available) === true;
			if (!edited && available) {
				ssid.value = typeof reportedConfig.ssid === 'string' ? reportedConfig.ssid : '';
				options.forEach(([ name ], index) => { flags[index].checked = meshd.flag(reportedConfig[name]) === true; });
			}

			dom.content(summaries, [
				...(localStatus ? [] : [ meshd.readError(status) ]),
				E('h3', [ _('Reported Roaming Configuration') ]),
				available ? meshd.facts([
					[ _('SSID'), meshd.text(reportedConfig.ssid) ],
					...options.map(([ name, label ]) => [ label, meshd.flagText(reportedConfig[name], _('Enabled'), _('Disabled')) ]),
					[ _('Reported configuration version'), meshd.text(reportedConfig.version) ]
				]) : reportedConfig ? E('p', [ _('No roaming configuration is available to report.') ]) : meshd.readError(roam),
				E('h3', [ _('Local Wireless Transaction') ]),
				transaction ? meshd.facts([
					[ _('State'), configState(transaction.state) ],
					[ _('Confirmed local version'), meshd.text(transaction.version) ],
					...(transaction.target_version == null ? [] : [ [ _('Transaction target version'), meshd.text(transaction.target_version) ] ]),
					[ _('Last error'), meshd.text(transaction.error) ]
				]) : meshd.readError(config)
			]);

			const actions = [];
			const gatewayAdmin = meshd.canManage(localStatus);
			if (!gatewayAdmin || !meshd.transportReady(localStatus))
				actions.push(E('p', [ meshd.managementHint(localStatus) ]));
			else if (reportedConfig && transaction)
				actions.push(editor);
			if (gatewayAdmin && isPending(transaction?.state))
				actions.push(E('p', [ rollback ]));
			dom.content(controls, actions);
			syncButtons();
		}

		async function submit() {
			if (busy || !meshd.canManage(localStatus) || !meshd.transportReady(localStatus) ||
			    !reportedConfig || !canStart(transaction?.state))
				return;
			const name = ssid.value;
			const password = key.value;
			if (!name.length || new TextEncoder().encode(name).length > 32 || /[\x00-\x1f\x7f-\x9f]/.test(name)) {
				feedback.textContent = _('The SSID must be 1–32 UTF-8 bytes with no control characters.');
				ssid.focus();
				return;
			}
			if (!/^(?:[\x20-\x7e]{8,63}|[0-9a-fA-F]{64})$/.test(password)) {
				feedback.textContent = _('Enter a valid Wi-Fi key before applying the configuration.');
				key.focus();
				return;
			}
			if (!window.confirm(_('Apply wireless settings to the preselected gateway APs? This may interrupt your connection.')))
				return;

			await run(async () => {
				try {
					await meshd.applyConfig(name, password, ...flags.map(input => input.checked));
				}
				catch {
					feedback.textContent = _('The apply request could not be confirmed. Refresh the transaction status and check diagnostics before retrying.');
					update(await this.load());
					return;
				}
				key.value = '';
				feedback.textContent = _('Apply request accepted. Refresh to check the local transaction and use the Nodes page to check reported versions.');
				update(await this.load());
			});
		}

		async function cancel() {
			if (busy || !meshd.canManage(localStatus) || !isPending(transaction?.state))
				return;
			if (!window.confirm(_('Roll back the unconfirmed local wireless transaction? This does not revert configurations already committed on the network.')))
				return;

			await run(async () => {
				try {
					await meshd.rollbackConfig();
					feedback.textContent = _('Rollback request accepted. Refresh to verify the local result.');
				}
				catch (error) {
					feedback.textContent = _('Rollback request failed: ') + meshd.text(error?.message);
				}
				update(await this.load());
			});
		}

		update(initial);
		return E('div', [
			E('h2', [ _('Mesh Roaming and Wireless') ]),
			E('p', [ _('Configure the APs that Mesh may manage before applying. LuCI does not change roles or backhaul here.') ]),
			E('p', [ refresh, ' ', E('a', { href: L.url('admin/network/mesh/nodes') }, [ _('View node versions') ]) ]),
			E('p', [ _('Local commit is not a network-wide acknowledgment. Compare the confirmed local version with the versions reported by nodes.') ]),
			summaries,
			controls,
			feedback
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
