// ffxiah.com snapshot scraper. Run in a browser tab on https://www.ffxiah.com (any page),
// via the browser's JS console or the Claude built-in browser's javascript tool.
// ffxiah refuses plain HTTP clients, so this runs as a normal visitor, one request per second.
// The server is whatever the site has selected (default Asura).
//
// 1. mktBrowse()      category pages -> median price, stock, sales/day per item (and stack)
//                     ~65 requests. Saves the queue for step 2 in localStorage.
// 2. mktSales()       item pages, busiest items first -> lowest sale price in the shown history.
//                     ~13k requests (~3.7 h). Resumable: progress is kept in localStorage.
//                     window.__crawl shows progress; set window.__crawl.running = false to stop.
// 3. await mktExport('browse') / await mktExport('sales')
//                     returns 'BEGIN<base64 gzip>END'; decode with
//                     python modules/bots/market/load_snapshot.py import-browse|import-sales <file>

const MKT_CATEGORIES = ("1/hand-to-hand 2/daggers 3/swords 4/great-swords 5/axes 6/great-axes 7/scythes 8/polearms " +
    "9/katana 10/great-katana 11/clubs 12/staves 13/ranged 14/instruments 47/fishing-gear 48/pet-items 15/ammunition " +
    "62/grips 16/shields 17/head 22/neck 18/body 19/hands 23/waist 20/legs 21/feet 26/back 24/earrings 25/rings " +
    "28/white-magic 29/black-magic 32/songs 31/ninjutsu 30/summoning 60/dice 45/geomancy 33/medicines 34/furnishings " +
    "44/alchemy 41/leathercraft 38/smithing 63/alchemy-2 40/clothcraft 43/woodworking 39/goldsmithing 42/bonecraft " +
    "52/meat-eggs 53/seafood 54/vegetables 55/soups 56/breads-rice 57/sweets 58/drinks 59/ingredients 51/fish " +
    "35/crystals 46/misc 50/beast-made 36/cards 49/ninja-tools 37/cursed-items 61/automatons 64/misc-2 65/misc-3").split(' ');

const mktSleep = ms => new Promise(r => setTimeout(r, ms));

function mktParseBrowse(doc) {
    const t = [...doc.querySelectorAll('table.stdlist')].sort((a, b) => b.rows.length - a.rows.length)[0];
    const num = s => parseInt((s || '').replace(/[^0-9]/g, '')) || 0;
    const rows = [];
    for (const tr of [...(t ? t.rows : [])].slice(1)) {
        const a = tr.cells[1]?.querySelector('a');
        if (!a) continue;
        const href = a.getAttribute('href');
        const m = href.match(/\/item\/(\d+)/);
        if (!m) continue;
        // itemid, stack, name, median gil, stock, sold/day
        rows.push([+m[1], /stack=1/.test(href) ? 1 : 0, a.textContent.trim(), num(tr.cells[4]?.innerText),
                   num(tr.cells[5]?.innerText), parseFloat(tr.cells[6]?.innerText) || 0]);
    }
    return rows;
}

async function mktBrowse() {
    const all = new Map();
    for (const path of MKT_CATEGORIES) {
        const html = await (await fetch('/browse/' + path)).text();
        for (const r of mktParseBrowse(new DOMParser().parseFromString(html, 'text/html'))) all.set(r[0] + ':' + r[1], r);
        await mktSleep(1000);
    }
    const rows = [...all.values()].filter(r => r[3] > 0 || r[4] > 0).sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    localStorage.setItem('mkt_browse', JSON.stringify(rows.map(r => [r[0], r[1], r[3], r[4], r[5]])));
    localStorage.setItem('mkt_queue', JSON.stringify(rows.filter(r => r[3] > 0).sort((a, b) => b[5] - a[5]).map(r => [r[0], r[1]])));
    return rows.length;
}

async function mktSales() {
    window.__crawl = { done: 0, err: 0, running: true, last: null };
    const q = JSON.parse(localStorage.getItem('mkt_queue'));
    const res = JSON.parse(localStorage.getItem('mkt_res') || '{}');
    let sinceSave = 0;
    for (const [id, stack] of q) {
        if (!window.__crawl.running) break;
        const key = id + ':' + stack;
        if (res[key]) { window.__crawl.done++; continue; }
        const t0 = Date.now();
        try {
            const r = await fetch('/item/' + id + (stack ? '?stack=1' : ''));
            if (r.status === 429 || r.status === 403) { window.__crawl.last = 'HTTP ' + r.status; await mktSleep(60000); continue; }
            const m = (await r.text()).match(/Item\.sales\s*=\s*(\[.*?\]);/s);
            const sales = m ? JSON.parse(m[1]) : [];
            const prices = sales.map(s => Number(s.price)).filter(p => p > 0);
            const times = sales.map(s => Number(s.saleon));
            // lowest sale, number of sales shown, newest and oldest sale (unix time)
            res[key] = prices.length ? [Math.min(...prices), prices.length, Math.max(...times), Math.min(...times)] : [0, 0, 0, 0];
            window.__crawl.done++;
            window.__crawl.last = key;
        } catch (e) { window.__crawl.err++; window.__crawl.last = 'ERR ' + e; }
        if (++sinceSave >= 25) { localStorage.setItem('mkt_res', JSON.stringify(res)); sinceSave = 0; }
        const wait = 1000 - (Date.now() - t0);
        if (wait > 0) await mktSleep(wait);
    }
    localStorage.setItem('mkt_res', JSON.stringify(res));
    window.__crawl.running = false;
}

async function mktExport(which) {
    const text = localStorage.getItem(which === 'sales' ? 'mkt_res' : 'mkt_browse');
    const gz = await new Response(new Blob([text]).stream().pipeThrough(new CompressionStream('gzip'))).arrayBuffer();
    let bin = '';
    for (const b of new Uint8Array(gz)) bin += String.fromCharCode(b);
    return 'BEGIN' + btoa(bin) + 'END';
}
