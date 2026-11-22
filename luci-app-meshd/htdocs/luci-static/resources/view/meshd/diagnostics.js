'use strict';
'require view';
'require dom';
'require meshd';

// SPDX-License-Identifier: GPL-2.0-or-later

function eventTime(seconds) {
	if (!Number.isFinite(seconds))
		return meshd.text(null);
	const date = new Date(seconds * 1000);
	return Number.isNaN(date.getTime()) ? meshd.text(null) : date.toLocaleString();
}

function checkStatus(value) {
	switch (value) {
	case 'PASS': return _('Passed');
	case 'WARN': return _('Warning');
	case 'FAIL': return _('Failed');
	default: return meshd.text(value);
	}
}

return view.extend({
	load() {
		return Promise.all([ meshd.read('diagnose'), meshd.read('event_log') ]);
	},

	render(initial) {
		const results = E('div');
		const update = ([ diagnosis, log ]) => {
			dom.content(results, [
				E('h3', [ _('Local Checks') ]),
				diagnosis.value ? meshd.table(
					[ _('Check'), _('Status'), _('Detail') ],
					meshd.items(diagnosis.value.checks).map(check => [
						meshd.text(check.name), checkStatus(check.status), meshd.text(check.detail)
					]),
					_('No checks reported.')
				) : meshd.readError(diagnosis),
				E('h3', [ _('Mesh Events') ]),
				log.value ? meshd.table(
					[ _('Time (browser local)'), _('Event'), _('Detail') ],
					meshd.items(log.value.events).map(event => [
						eventTime(event.time), meshd.text(event.type), meshd.text(event.detail)
					]),
					_('No events reported.')
				) : meshd.readError(log)
			]);
		};
		const refresh = meshd.button(_('Refresh'), async () => {
			refresh.disabled = true;
			try {
				update(await this.load());
			}
			finally {
				refresh.disabled = false;
			}
		});
		update(initial);

		return E('div', [
			E('h2', [ _('Mesh Diagnostics') ]),
			E('p', [ refresh ]),
			results
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
