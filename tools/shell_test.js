// Tests for the parts of web/shell.html that only misbehave on a phone.
//
// The shell is one file of inline script with no module boundary, so each
// function under test is pulled out of the page by name and run against a
// stub -- which means the test cannot drift from what actually ships, and
// needs neither a browser nor a ROM.
//
//   overStage()    -- decides whether a touch is on the picture (and so gets
//                     preventDefault()) or on page UI (and so must not).
//                     Getting this wrong takes the tap, the scroll and the
//                     selection away from whatever it wrongly claimed.
//   fallbackCopy() -- the execCommand path, which is what iOS actually uses
//                     whenever the page is not on a secure origin (a plain
//                     http:// LAN address, say). navigator.clipboard does
//                     not exist there, so this is not a rare branch.
//
// The stubs model iOS's actual quirks rather than the spec: a readonly field
// ignores select() and setSelectionRange, an unselectable one likewise, and
// execCommand('copy') returns true regardless -- it reports that the command
// ran, not that anything reached the clipboard. That last one is why this
// file exists: it makes a failure to copy indistinguishable from a success
// unless the caller checks the selection itself.
//
//   node tools/shell_test.js [web/shell.html]

const fs = require('fs');
const src = fs.readFileSync(process.argv[2] || 'web/shell.html', 'utf8');

let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  ok   ' + name); }
  else { fail++; console.log('  FAIL ' + name + (extra ? '  (' + extra + ')' : '')); }
}

function extract(name) {
  const i = src.indexOf('function ' + name + '(');
  if (i < 0) throw new Error('cannot find ' + name);
  let depth = 0, started = false, j = i;
  for (; j < src.length; j++) {
    if (src[j] === '{') { depth++; started = true; }
    else if (src[j] === '}') { depth--; if (started && depth === 0) { j++; break; } }
  }
  return src.slice(i, j);
}

//--------------------------------------------------------------------
// overStage
//--------------------------------------------------------------------
console.log('overStage');
const overStage = new Function('return (' + extract('overStage') + ')')();

function chain(...ids) {                 // innermost first
  let node = null;
  for (const id of ids) node = { nodeType: 1, id, parentNode: node };
  // built outward; rebuild so ids[0] is the leaf
  let out = null;
  for (let i = ids.length - 1; i >= 0; i--) out = { nodeType: 1, id: ids[i], parentNode: out };
  return out;
}

ok('canvas in the stage is stage',        overStage(chain('screen', 'screen-wrap')) === true);
ok('a touch pad is stage',                overStage(chain('a-btn', 'touch')) === true);
ok('the stage itself is stage',           overStage(chain('screen-wrap')) === true);
ok('crash report text is NOT stage',      overStage(chain('crashreport', 'crash', 'screen-wrap')) === false);
ok('Copy report button is NOT stage',     overStage(chain('crashcopy', 'crashbtns', 'crash', 'screen-wrap')) === false);
ok('curtain content is NOT stage',        overStage(chain('romfile-label', 'curtain', 'screen-wrap')) === false);
ok('diag HUD is still stage',             overStage(chain('diag', 'screen-wrap')) === true);
ok('the sheet is not stage',              overStage(chain('logcopy', 'chrome')) === false);
ok('null target is not stage',            overStage(null) === false);

//--------------------------------------------------------------------
// fallbackCopy
//--------------------------------------------------------------------
console.log('fallbackCopy');

// A DOM stub just deep enough for this one function. execCommandCopies is
// what a real clipboard would have received: the *selected* text, which is
// the whole point -- execCommand returns true either way.
function makeDom(opts) {
  opts = opts || {};
  const doc = {
    activeElement: { focus() { doc._refocused = true; } },
    body: {
      children: [],
      appendChild(el) { doc.body.children.push(el); el._inDocument = true; },
      removeChild(el) {
        const i = doc.body.children.indexOf(el);
        if (i < 0) throw new Error('removeChild: not a child');
        doc.body.children.splice(i, 1);
        el._inDocument = false;
      },
    },
    createElement() {
      const el = {
        tagName: 'TEXTAREA', value: '', style: { cssText: '' },
        attrs: {}, selectionStart: 0, selectionEnd: 0,
        setAttribute(k, v) { el.attrs[k] = v; },
        focus() {},
        // Same rules as setSelectionRange below -- on iOS this is the call
        // that is silently ignored, which is the whole reason the old code
        // followed it with setSelectionRange in the first place.
        select() { el.setSelectionRange(0, el.value.length); },
        setSelectionRange(a, b) {
          // The real iOS bug: a readonly field ignores this outright.
          if (el.readOnly || 'readonly' in el.attrs) return;
          // ...and so does one whose text cannot be selected at all.
          if (/user-select\s*:\s*none/.test(el.style.cssText)) return;
          el.selectionStart = a; el.selectionEnd = b;
        },
      };
      doc._made = el;
      return el;
    },
    createRange() {
      return { selectNodeContents(el) { this._el = el; } };
    },
    execCommand(cmd) {
      if (cmd !== 'copy') return false;
      // Faithful to the browser: the command ran. Whether anything reached
      // the clipboard depends entirely on there being a selection.
      const el = doc._made;
      const n = el.selectionEnd - el.selectionStart;
      doc.clipboard = n > 0 ? el.value.slice(el.selectionStart, el.selectionEnd) : null;
      return opts.execCommandFails ? false : true;
    },
    clipboard: null,
  };
  const selection = {
    ranges: [],
    rangeCount: 0,
    getRangeAt(i) { return selection.ranges[i]; },
    removeAllRanges() { selection.ranges = []; selection.rangeCount = 0; },
    addRange(r) { selection.ranges.push(r); selection.rangeCount = selection.ranges.length; },
  };
  return { doc, selection };
}

function runCopy(text, opts) {
  const { doc, selection } = makeDom(opts);
  const fn = new Function('document', 'window', 'return (' + extract('fallbackCopy') + ')')(
    doc, { getSelection: () => selection });
  const ret = fn(text);
  return { ret, doc, selection };
}

const REPORT = 'katam-port crash report\nline two\nline three';

let r = runCopy(REPORT);
ok('returns true when the text really was copied', r.ret === true);
ok('the clipboard got the whole report', r.doc.clipboard === REPORT,
   JSON.stringify(r.doc.clipboard));
ok('the textarea is not readonly',
   !('readonly' in r.doc._made.attrs) && r.doc._made.readOnly === false);
ok('the textarea re-enables user-select',
   /(^|;)\s*user-select\s*:\s*text/.test(r.doc._made.style.cssText) &&
   /-webkit-user-select\s*:\s*text/.test(r.doc._made.style.cssText),
   r.doc._made.style.cssText);
ok('font-size is 16px so iOS does not zoom',
   /font-size\s*:\s*16px/.test(r.doc._made.style.cssText));
ok('the scratch textarea is removed again', r.doc.body.children.length === 0);
ok('focus is handed back', r.doc._refocused === true);

// The regression itself: a selection that never took must not be reported as
// a success just because execCommand returned true.
r = runCopy(REPORT, {});
const el = r.doc._made;
ok('empty selection would be caught',
   (function () {
     // Re-run with a textarea that refuses to be selected, exactly as one
     // inheriting `user-select: none` from body did.
     const { doc, selection } = makeDom();
     const realCreate = doc.createElement;
     doc.createElement = function () {
       const e = realCreate();
       e.style = { cssText: '' };
       Object.defineProperty(e.style, 'cssText', {
         get() { return 'user-select:none'; }, set() {}, configurable: true });
       return e;
     };
     const fn = new Function('document', 'window', 'return (' + extract('fallbackCopy') + ')')(
       doc, { getSelection: () => selection });
     return fn(REPORT) === false && doc.clipboard === null;
   })());

ok('a refusing execCommand reports failure', runCopy(REPORT, { execCommandFails: true }).ret === false);

// The player's own selection has to survive the attempt.
(function () {
  const { doc, selection } = makeDom();
  const mine = { mine: true };
  selection.addRange(mine);
  const fn = new Function('document', 'window', 'return (' + extract('fallbackCopy') + ')')(
    doc, { getSelection: () => selection });
  fn(REPORT);
  ok('the pre-existing selection is restored',
     selection.rangeCount === 1 && selection.ranges[0] === mine);
})();

//--------------------------------------------------------------------
// The CSS half
//--------------------------------------------------------------------
console.log('CSS');
const cssRule = src.match(/#log, #crashreport, input \{([^}]*)\}/);
ok('the selectable elements exist as a rule', !!cssRule);
ok('long-press selection is re-enabled on iOS',
   !!cssRule && /-webkit-touch-callout:\s*default/.test(cssRule[1]));
ok('user-select is still re-enabled',
   !!cssRule && /-webkit-user-select:\s*text/.test(cssRule[1]));

//--------------------------------------------------------------------
// The whole page parses
//
// Cheap, and the shell is a single hand-edited file with five inline script
// blocks: a stray brace in one of them takes the page down with no build
// step anywhere to have caught it.
//--------------------------------------------------------------------
console.log('syntax');
const blocks = src.match(/<script(?![^>]*\bsrc=)[^>]*>[\s\S]*?<\/script>/g) || [];
ok('found the inline script blocks', blocks.length >= 4, blocks.length + ' found');
blocks.forEach(function (b, i) {
  const body = b.replace(/^<script[^>]*>/, '').replace(/<\/script>$/, '');
  let err = null;
  try { new Function(body); } catch (e) { err = e.message; }
  ok('script block ' + i + ' parses', err === null, err);
});

console.log('\n' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
