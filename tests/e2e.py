#!/usr/bin/env python3
# web-vmu tests - drive the real app in a phone-sized Chromium (needs: pip install playwright).
# Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tests/e2e.py [url] [screenshot dir] [folder with extra .vms/.vmi/.dci files to import]
#
# Serves nothing itself: start the site first, e.g.  bash run.sh
# Checks: empty library, the built-in test game, the screen, keyboard and touch input, sound,
# saving to the memory card, snapshots (leave and continue), MODE, importing every file format,
# downloading a card, deleting files.
import sys, os, io, time, zipfile, glob

def main():
    from playwright.sync_api import sync_playwright
    url = sys.argv[1] if len(sys.argv) > 1 else 'http://localhost:8768/'
    shots = sys.argv[2] if len(sys.argv) > 2 else None
    extra = sys.argv[3] if len(sys.argv) > 3 else None
    if shots: os.makedirs(shots, exist_ok=True)
    fails = []
    def check(name, ok, detail=''):
        print(('ok    ' if ok else 'FAIL  ') + name + (('  ' + str(detail)) if detail != '' else ''))
        if not ok: fails.append(name)
    def shot(pg, name):
        if shots: pg.screenshot(path=os.path.join(shots, name + '.png'))

    with sync_playwright() as p:
        b = p.chromium.launch(args=['--autoplay-policy=no-user-gesture-required'])
        ctx = b.new_context(viewport={'width': 390, 'height': 844}, device_scale_factor=2, has_touch=True, is_mobile=True)
        pg = ctx.new_page()
        errors = []
        pg.on('pageerror', lambda e: errors.append(str(e)))
        pg.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)
        P = 'window.webVmu.player'
        lit = lambda: pg.evaluate("() => { const f = %s.m.frame(); let n = 0; for (const v of f.pixels) n += v; return f.on ? n : -1; }" % P)
        status = lambda: pg.inner_text('#lib-status')

        pg.goto(url); pg.wait_for_function("document.querySelector('#lib-status').textContent.includes('stored on this device')")
        check('an empty library explains itself', pg.locator('#empty-banner:not(.hidden)').count() == 1 and pg.locator('.game').count() == 0)
        shot(pg, '1-library-empty')

        # --- the built-in test game
        pg.click('#btn-demo'); pg.wait_for_selector('.game')
        check('the built-in test game becomes a card with a game', status() == '1 game on 1 memory card', status())
        check('its icon is read from the game file', pg.locator('.game .thumb img').count() == 1)
        check('the card lists its files and free space', '196 of 200 blocks free' in pg.inner_text('.card .meta'), pg.inner_text('.card .meta'))

        pg.click('.game .name'); pg.wait_for_function("%s.m && %s.raf" % (P, P)); time.sleep(0.6)
        check('the game starts and draws', lit() == 1, '%d dot(s) lit' % lit())
        check('the game icon is lit under the screen', pg.locator('#ic-game.on').count() == 1 and pg.locator('#ic-file.on').count() == 0)

        # --- keyboard: draw a line to the right, then down
        pg.keyboard.down('ArrowRight'); time.sleep(0.5); pg.keyboard.up('ArrowRight')
        a = lit()
        pg.keyboard.down('ArrowDown'); time.sleep(0.4); pg.keyboard.up('ArrowDown'); time.sleep(0.15)
        c = lit()
        check('keyboard arrows move the pen', a > 5 and c > a + 3, '%d then %d dots' % (a, c))

        # --- touch: the direction pad (left half) and a quick tap
        box = pg.locator('#dpad').bounding_box()
        pg.touchscreen.tap(box['x'] + box['width'] * 0.12, box['y'] + box['height'] * 0.5); time.sleep(0.25)
        d = lit()
        check('a tap on the direction pad moves the pen', d == c + 1 or d == c + 2, '%d -> %d dots' % (c, d))
        shot(pg, '2-playing')

        # --- B saves the picture to the memory card
        before = pg.evaluate("window.webVmu.cards[0].modified")
        pg.keyboard.down('x'); time.sleep(0.2); pg.keyboard.up('x')
        heard = pg.evaluate("(() => { const a = %s.audio; if (!a) return -1; let n = 0; for (const v of a.ring) if (v) n++; return n; })()" % P)
        time.sleep(1.8)
        saved = pg.evaluate("window.webVmu.db.get('card:' + window.webVmu.cards[0].id).then(b => { let n = 0; for (let i = 0x600; i < 0x700; i++) n += b[i] ? 1 : 0; return n; })")
        check('B writes the picture to the card, and the card is stored', saved > 0 and pg.evaluate("window.webVmu.cards[0].modified") > before, '%d bytes of picture in the save area' % saved)
        check('the buzzer is heard', heard > 500, '%d sound samples' % heard)

        # --- leave and continue from the snapshot
        pg.click('#btn-back'); pg.wait_for_selector('#library.active'); time.sleep(0.2)
        check('leaving saves a snapshot', 'saved just now' in pg.inner_text('.game .meta'), pg.inner_text('.game .meta'))
        shot(pg, '3-library')
        pg.click('.game .name'); pg.wait_for_function("%s.m && %s.raf" % (P, P)); time.sleep(0.4)
        check('coming back continues where it was', lit() == d and 'Continued' in pg.inner_text('#toast'), '%d dots' % lit())

        # --- A wipes the screen; MODE ends the game; playing again brings the saved picture back from the card
        btn_a = pg.locator('.pbtn[data-btn="16"]').bounding_box()
        pg.touchscreen.tap(btn_a['x'] + btn_a['width'] / 2, btn_a['y'] + btn_a['height'] / 2); time.sleep(0.4)
        check('a tap on A wipes the screen', lit() == 1, '%d dot(s)' % lit())
        pg.keyboard.press('Enter'); pg.wait_for_selector('#sheet:not(.hidden)')
        check('MODE ends the game', 'ended' in pg.inner_text('#sheet-title'))
        pg.click('#sheet-actions .btn.primary'); time.sleep(0.6)
        check('playing again loads the picture saved on the card', lit() == d, '%d dots' % lit())
        pg.click('#btn-back'); pg.wait_for_selector('#library.active')

        # --- download the card, then import that same file as a new card
        pg.click('.card .more'); pg.click("#sheet-actions .btn:has-text('Download the card')"); time.sleep(0.3)
        card = bytes(pg.evaluate("Array.from(window.webVmu.lastDownload.bytes)"))
        check('a card downloads as a 128 KiB image', len(card) == 131072 and pg.evaluate("window.webVmu.lastDownload.name") == 'Sketch.bin')
        pg.set_input_files('.import-row .import-input', [{'name': 'vmu_save_A1.bin', 'mimeType': 'application/octet-stream', 'buffer': card}])
        pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('2 games on 2')")
        check('a raw card image imports as a card', True, pg.inner_text('#toast'))
        dcm = bytearray(card)
        for i in range(0, len(dcm), 4): dcm[i:i + 4] = dcm[i:i + 4][::-1]
        pg.set_input_files('.import-row .import-input', [{'name': 'backup.dcm', 'mimeType': 'application/octet-stream', 'buffer': bytes(dcm)}])
        pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('3 games on 3')")
        check('a .dcm card image imports as a card', True, pg.inner_text('#toast'))
        pg.set_input_files('.import-row .import-input', [{'name': 'notes.bin', 'mimeType': 'application/octet-stream', 'buffer': b'\x01' * 131072}])
        pg.wait_for_selector('#sheet:not(.hidden)')
        check('a 128 KiB file that is not a card is refused', 'not a formatted VMU card' in pg.inner_text('#sheet-body') and status().startswith('3 games on 3'))
        pg.click('#sheet-actions .btn')
        pg.set_input_files('.import-row .import-input', [{'name': 'bios.bin', 'mimeType': 'application/octet-stream', 'buffer': b'\x02' * 65536}])
        pg.wait_for_selector('#sheet:not(.hidden)')
        check('a file of some other kind is refused', 'is not a memory card' in pg.inner_text('#sheet-body') and status().startswith('3 games on 3'))
        pg.click('#sheet-actions .btn')

        # --- other people's files, if a folder of them was given: loose .vms, .vms + .vmi, .dci, and a zip
        n_games = 3
        if extra:
            vms = sorted(glob.glob(os.path.join(extra, '**', '*.[vV][mM][sS]'), recursive=True))
            dci = sorted(glob.glob(os.path.join(extra, '**', '*.[dD][cC][iI]'), recursive=True))
            for path in vms[:6]:
                files = [path] + [v for v in (path[:-1] + 'I', path[:-1] + 'i') if os.path.exists(v)][:1]
                pg.set_input_files('.import-row .import-input', files); n_games += 1
                pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('%d games')" % n_games)
                check('import ' + ' + '.join(os.path.basename(f) for f in files), True, pg.inner_text('#toast'))
            for path in dci[:2]:
                pg.set_input_files('.import-row .import-input', [path]); n_games += 1
                pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('%d games')" % n_games)
                check('import ' + os.path.basename(path), True, pg.inner_text('#toast'))
            if vms:
                buf = io.BytesIO()
                with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
                    z.write(vms[0], 'games/' + os.path.basename(vms[0])); z.writestr('games/readme.txt', 'hello')
                pg.set_input_files('.import-row .import-input', [{'name': 'games.zip', 'mimeType': 'application/zip', 'buffer': buf.getvalue()}]); n_games += 1
                pg.wait_for_function("document.querySelector('#lib-status').textContent.startsWith('%d games')" % n_games)
                check('import a zip', True, pg.inner_text('#toast'))
            shot(pg, '4-library-full')
            # every imported game must start and show something
            titles = pg.locator('.game .name').all_inner_texts()
            for i, t in enumerate(titles):
                pg.locator('.game .name').nth(i).click(); pg.wait_for_function("%s.m && %s.raf" % (P, P)); time.sleep(1.2)
                n = lit()
                check('%s runs' % t, n != 0 and not pg.evaluate("%s.over" % P), '%s dots lit' % ('screen off,' if n < 0 else n))
                if t not in titles[:i] and t != 'Sketch': shot(pg, '5-playing-' + ''.join(ch for ch in t if ch.isalnum()))
                pg.click('#btn-back'); pg.wait_for_selector('#library.active')

        # --- files on a card: download one, delete it
        pg.locator('.card .more').first.click(); pg.click("#sheet-actions .btn:has-text('Files on this card')"); pg.wait_for_selector('.files')
        shot(pg, '6-files')
        pg.click('.files [data-get]'); time.sleep(0.2)
        check('a file downloads as a .vms', pg.evaluate("window.webVmu.lastDownload.name").endswith('.vms') and pg.evaluate("window.webVmu.lastDownload.bytes.length") % 512 == 0, pg.evaluate("window.webVmu.lastDownload.name"))
        pg.click('.files [data-del]'); pg.wait_for_function("document.querySelector('#sheet-body').textContent.includes('empty')")
        pg.click('#sheet-actions .btn'); time.sleep(0.2)
        check('deleting the game frees the card', status().startswith('%d game' % (n_games - 1)) and any('200 of 200 blocks free' in t for t in pg.locator('.card .meta').all_inner_texts()), status())

        # --- everything survives a reload
        pg.reload(); pg.wait_for_function("document.querySelector('#lib-status').textContent.includes(' on ')")
        check('the library survives a reload', status().startswith('%d game' % (n_games - 1)), status())
        check('no errors in the browser console', not errors, errors[:3])
        b.close()
    print('\n%s' % ('ALL CHECKS PASSED' if not fails else '%d CHECK(S) FAILED: %s' % (len(fails), ', '.join(fails))))
    sys.exit(1 if fails else 0)

if __name__ == '__main__': main()
