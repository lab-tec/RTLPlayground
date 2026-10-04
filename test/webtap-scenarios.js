/*
 * Browser scenarios against test/webtap.c: the firmware's real web stack on a
 * TAP interface. Two separate browsers stand in for two machines (their own
 * cookies and connection pools), the way the switch saw the Mac and
 * desktoplinux on 2026-10-04.
 *
 *   test/build/webtap <firmware.bin> &      (as root; WEBTAP_CPU_US slows it)
 *   node test/webtap-scenarios.js <firmware.bin>
 *   SCENARIOS=lossy node ...   (with webtap started as "webtap <image> 10")
 *
 * Each scenario prints what happened; nothing here asserts a firmware's
 * behaviour is right, so the same script measures before and after a change.
 */
const { chromium } = require('playwright');

const BASE = 'http://10.77.0.2';
const IMAGE = process.argv[2];

async function browser() {
	return chromium.launch({ args: ['--no-proxy-server'] });
}

async function login(page) {
	const t0 = Date.now();
	await page.goto(BASE + '/login.html', { timeout: 60000 });
	await page.fill('#pwd', '1234');
	await Promise.all([
		page.waitForURL(/index\.html/, { timeout: 60000 }),
		page.press('#pwd', 'Enter'),
	]);
	await page.waitForFunction(() =>
		document.querySelectorAll('#navlist li').length > 0 &&
		document.getElementById('brandname').textContent !== 'Switch',
		null, { timeout: 60000 });
	return Date.now() - t0;
}

async function onLoginPage(page) {
	return /login\.html/.test(page.url());
}

async function twoLogins() {
	const a = await browser(), b = await browser();
	const pa = await a.newPage(), pb = await b.newPage();
	console.log('\n== Two browsers, one after the other');
	console.log('  A logged in and ready in', await login(pa), 'ms');
	console.log('  B logged in and ready in', await login(pb), 'ms');
	await pa.waitForTimeout(8000);
	console.log('  8 s later, A is', await onLoginPage(pa) ? 'LOGGED OUT' : 'still logged in',
		'and B is', await onLoginPage(pb) ? 'LOGGED OUT' : 'still logged in');
	const note = await pa.locator('#note, .note, #lmsg').first().textContent().catch(() => null);
	if (note)
		console.log('  A\'s login page says:', JSON.stringify(note.trim()));
	await a.close(); await b.close();
}

async function loadWhilePolling() {
	const a = await browser(), b = await browser();
	const pb = await b.newPage();
	console.log('\n== A logs in while B\'s Dashboard polls');
	await login(pb);
	const times = [];
	for (let i = 0; i < 3; i++) {
		const ctx = await a.newContext(), pa = await ctx.newPage();
		try {
			times.push(await login(pa));
		} catch (e) {
			times.push('failed: ' + String(e).split('\n')[0]);
		}
		await ctx.close();
	}
	console.log('  A ready after (ms):', times.join(', '));
	await a.close(); await b.close();
}

async function uploadAfterReplaced() {
	if (!IMAGE)
		return;
	const a = await browser(), b = await browser();
	const pa = await a.newPage(), pb = await b.newPage();
	console.log('\n== A uploads firmware after B has logged in');
	await login(pa);
	await pa.goto(BASE + '/index.html#fw');
	await pa.setInputFiles('#fwfile', IMAGE);
	await pa.waitForFunction(() => !document.getElementById('fwup').disabled, null, { timeout: 30000 });
	await login(pb);
	await pa.click('#fwup');
	await pa.click('#mfoot .pri');
	await pa.waitForFunction(() => {
		const s = document.getElementById('fwstat');
		return s && /✕|rejected|logged|session|verified|lost/i.test(s.textContent);
	}, null, { timeout: 120000 }).catch(() => {});
	console.log('  A\'s Firmware page says:',
		JSON.stringify((await pa.textContent('#fwstat').catch(() => '')).trim()),
		await onLoginPage(pa) ? '(and A is on the login page)' : '');
	await a.close(); await b.close();
}

async function pushedOut() {
	if (!IMAGE)
		return;
	const a = await browser();
	const pa = await a.newPage();
	console.log('\n== A uploads after four other browsers have logged in');
	await login(pa);
	await pa.goto(BASE + '/index.html#fw');
	await pa.setInputFiles('#fwfile', IMAGE);
	await pa.waitForFunction(() => !document.getElementById('fwup').disabled, null, { timeout: 30000 });
	for (let i = 0; i < 4; i++) {
		const o = await browser();
		await login(await o.newPage());
		await o.close();
	}
	await pa.click('#fwup');
	await pa.click('#mfoot .pri');
	await pa.waitForFunction(() => /\u2715|verified|lost/.test(document.getElementById('fwstat').textContent),
		null, { timeout: 120000 }).catch(() => {});
	console.log('  A\'s Firmware page says:', JSON.stringify((await pa.textContent('#fwstat')).trim()));
	await pa.goto(BASE + '/index.html');
	await pa.waitForURL(/login\.html/, { timeout: 30000 }).catch(() => {});
	console.log('  reloading, A lands on', pa.url().replace(BASE, ''), 'saying',
		JSON.stringify((await pa.textContent('#err').catch(() => '')).trim()));
	await a.close();
}

/* Run against "webtap <image> <loss-percent>": fresh logins over a lossy link */
async function lossy() {
	const a = await browser();
	console.log('\n== Five fresh logins over a lossy link');
	const out = [];
	for (let i = 0; i < 5; i++) {
		const ctx = await a.newContext(), p = await ctx.newPage();
		const failed = [];
		p.on('requestfailed', r => failed.push(r.url().replace(BASE, '') + ' ' + r.failure().errorText));
		p.on('response', r => { if (r.status() >= 400) failed.push(r.url().replace(BASE, '') + ' HTTP ' + r.status()); });
		try {
			out.push(await login(p) + ' ms');
		} catch (e) {
			out.push('FAILED at ' + p.url().replace(BASE, '') +
				(failed.length ? ' [' + failed.join('; ') + ']' : ''));
		}
		await ctx.close();
	}
	console.log('  ' + out.join(',\n  '));
	await a.close();
}

const RUN = { two: twoLogins, poll: loadWhilePolling, upload: uploadAfterReplaced,
	      pushed: pushedOut, lossy: lossy };
(async () => {
	for (const name of (process.env.SCENARIOS || 'two,poll,upload,pushed').split(','))
		await RUN[name]();
})().catch(e => { console.error(e); process.exit(1); });
