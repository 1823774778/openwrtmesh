'use strict';
'require view';
'require dom';
'require meshd';

// SPDX-License-Identifier: GPL-2.0-or-later

function sessionState(value) {
	switch (value) {
	case 'online': return _('Online');
	case 'suspect': return _('Suspect');
	case 'offline': return _('Offline');
	case 'candidate': return _('Candidate');
	default: return meshd.text(value);
	}
}

return view.extend({
	load() {
		return Promise.all([ meshd.read('status'), meshd.read('topology') ]);
	},

	render(initial) {
		let localStatus, busy = false;
		const mayChange = () => meshd.canManage(localStatus) && meshd.transportReady(localStatus);
		const listing = E('div');
		const management = E('div');
		const feedback = E('p', { role: 'status', 'aria-live': 'polite' });
		const fingerprint = E('input', {
			id: 'meshd-node-id', type: 'text', class: 'cbi-input-text', spellcheck: 'false',
			autocomplete: 'off', autocapitalize: 'none', size: 52
		});
		const actions = E('fieldset', { class: 'cbi-section' }, [
			E('legend', [ _('Manage Node Authorization') ]),
			E('p', [ _('Before authorizing, compare the SHA-256 fingerprint with the fingerprint displayed on the candidate device using a trusted channel.') ]),
			E('p', [ E('label', { for: 'meshd-node-id' }, [ _('Node fingerprint') ]), ' ', fingerprint ]),
			E('p', [
				meshd.button(_('Authorize…'), () => changeAuthorization(true, fingerprint.value), 'positive'), ' ',
				meshd.button(_('Remove…'), () => changeAuthorization(false, fingerprint.value), 'negative')
			])
		]);
		const refresh = meshd.button(_('Refresh'), () => run(async () => update(await this.load())));

		function setBusy(value) {
			busy = value;
			refresh.disabled = value;
			actions.disabled = value;
			listing.querySelectorAll('button').forEach(button => { button.disabled = value; });
		}

		async function run(action) {
			if (busy)
				return;
			setBusy(true);
			try {
				await action();
			}
			finally {
				setBusy(false);
			}
		}

		const changeAuthorization = async (authorize, rawId) => {
			if (busy || !mayChange())
				return;
			const id = rawId.trim().toLowerCase();
			if (!/^[0-9a-f]{64}$/.test(id)) {
				feedback.textContent = _('Enter the complete 64-character hexadecimal fingerprint.');
				fingerprint.focus();
				return;
			}
			const prompt = authorize
				? _('Authorize the following verified fingerprint on this gateway?')
				: _('Remove authorization for the following fingerprint?');
			if (!window.confirm(prompt + '\n\n' + id))
				return;

			await run(async () => {
				try {
					if (authorize)
						await meshd.enroll(id);
					else
						await meshd.removeNode(id);
				}
				catch (error) {
					feedback.textContent = _('Node authorization request failed: ') + meshd.text(error?.message);
					return;
				}
				feedback.textContent = authorize
					? _('Authorization request accepted. An authorized node may not be connected yet.')
					: _('Removal request accepted. Check the node list for updated authorization.');
				if (fingerprint.value.trim().toLowerCase() === id)
					fingerprint.value = '';
				update(await this.load());
			});
		};

		function update([ status, topology ]) {
			localStatus = status.value;
			const writable = mayChange();
			dom.content(management, [ writable ? actions : E('p', [ meshd.managementHint(localStatus) ]) ]);
			const headers = [ _('Node fingerprint'), _('Role'), _('IP address'), _('Model'), _('Session state'), _('Authorized'), _('Reported version') ];
			if (writable)
				headers.push(_('Action'));
			const rows = meshd.items(topology.value?.nodes).map(node => {
				const approval = meshd.flag(node.approved);
				const values = [
					E('code', [ meshd.text(node.id) ]), meshd.role(node.role), meshd.text(node.ip), meshd.text(node.model),
					sessionState(node.state), meshd.flagText(node.approved, _('Yes'), _('No')),
					meshd.flag(node.config_failed) === true
						? E('span', [ meshd.text(node.version), ' · ', _('Apply failed') ])
						: meshd.text(node.version)
				];
				if (writable) {
					const valid = typeof node.id === 'string' && /^[0-9a-f]{64}$/.test(node.id);
					values.push(valid && approval !== null ? meshd.button(
						approval ? _('Remove…') : _('Authorize…'),
						() => changeAuthorization(!approval, node.id), approval ? 'negative' : 'positive'
					) : meshd.text(null));
				}
				return values;
			});
			dom.content(listing, [ topology.value ? meshd.table(headers, rows, _('No nodes reported.')) : meshd.readError(topology) ]);
			if (!status.value)
				management.prepend(meshd.readError(status));
		}

		update(initial);
		return E('div', [
			E('h2', [ _('Mesh Nodes') ]),
			E('p', [ _('This is the gateway’s view of direct control sessions and unapproved candidates. It does not show physical links or wireless hop paths.') ]),
			E('p', [ refresh ]),
			listing,
			management,
			feedback
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
