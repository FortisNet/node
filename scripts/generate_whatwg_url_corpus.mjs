#!/usr/bin/env node
// Freezes `new URL(input, base?)` as the production TypeScript runtime runs
// it, for src/compat/whatwg_url.cpp. Run inside the pinned image (no dependencies):
//
//   docker run --rm -v "$PWD/scripts:/s:ro" \
//     node:22-alpine@sha256:fe4f6d74e20370acb04ad889077d14d0f64a9c8ea6a42bc60cddf1e844699914 \
//     node /s/generate_whatwg_url_corpus.mjs > tests/corpus/whatwg_url_corpus.json
//   shasum -a 256 tests/corpus/whatwg_url_corpus.json | cut -d' ' -f1 \
//     > tests/corpus/whatwg_url_corpus.json.sha256
//
// Output (JSON):
//   cases:     curated inputs with every URL getter, or null when it throws;
//   canonical: the news clusterer's canonicalUrl over http(s) links;
//   exhaustive.contexts[i]: for every scalar value U+0080..U+10FFFF except
//     surrogates, in ascending order, one line
//       `${codePoint.toString(16)} ${href ?? 'null'}\n`
//     for the input `${prefix}${String.fromCodePoint(cp)}${suffix}`; lines are
//     hashed per block of 4096 code points (block = cp >> 12) and in total.
//     A context with `before`/`after` instead parses
//       `${prefix}${punycode(before + String.fromCodePoint(cp) + after)}${suffix}`
//     (RFC 3492 encoding below), driving the xn-- decode-and-validate path.
import { createHash } from 'node:crypto';

const IMAGE = 'node:22-alpine@sha256:fe4f6d74e20370acb04ad889077d14d0f64a9c8ea6a42bc60cddf1e844699914';

function describe(input, base) {
  let url;
  try {
    url = base === undefined ? new URL(input) : new URL(input, base);
  } catch {
    return null;
  }
  return {
    href: url.href, protocol: url.protocol, username: url.username, password: url.password,
    host: url.host, hostname: url.hostname, port: url.port, pathname: url.pathname,
    search: url.search, hash: url.hash,
  };
}

// The news clusterer's canonicalUrl (only the `new URL` branch is frozen;
// a throw is recorded as null and the caller applies the trim/lowercase).
function canonicalUrl(url) {
  try {
    const parsed = new URL(url);
    parsed.hash = '';
    parsed.protocol = 'https:';
    parsed.hostname = parsed.hostname.toLowerCase().replace(/^www\./, '');
    for (const key of [...parsed.searchParams.keys()]) {
      if (/^(?:utm_|ref$|source$|fbclid$|gclid$|mc_)/i.test(key)) parsed.searchParams.delete(key);
    }
    const path = parsed.pathname.replace(/\/+$/, '');
    const query = parsed.searchParams.toString();
    return `${parsed.hostname}${path}${query === '' ? '' : `?${query}`}`;
  } catch {
    return null;
  }
}

const hosts = [
  'example.com', 'EXAMPLE.com', 'ex%41mple.com', 'ex%zzample.com', 'ex%2ample.com', 'a..b', 'a.b.',
  '.', '..', 'a%2eb', '%2e', 'a%ffb', '%ff', '%c3%a9', '%C3%A9xample', '%e2%80%8d', '%00', 'a%20b',
  '127.0.0.1', '127.1', '127.0.1', '0x7f.1', '0x7F.0.0.1', '0x7f000001', '2130706433', '017700000001',
  '0177.0.0.1', '0.0.0.0', '0', '00', '0x', '0x.0x.0x.0x', '1.2.3.4.', '1.2.3.4..', '1.2.3.256',
  '1.2.3.0x100', '256.0.0.1', '4294967295', '4294967296', '0x100000000', '1.2.3', '1.2', 'a.b.c.1',
  'foo.09', 'foo.0x', 'foo.0xg', 'foo.1e', '09', '08.1', '1.08', '0x1.0x2.0x3.0x4', '1.0x', 'x.0x',
  '169.254.169.254', '10.0.0.1', '192.168.0.1', '100.64.0.1', '[::1]', '[::]', '[::ffff:127.0.0.1]',
  '[::ffff:7f00:1]', '[64:ff9b::7f00:1]', '[64:ff9b::127.0.0.1]', '[2001:db8::1]', '[fe80::1]',
  '[1:2:3:4:5:6:7:8]', '[1:2:3:4:5:6:7:8:9]', '[1::2::3]', '[::1.2.3.4]', '[::1.2.3]', '[::01.2.3.4]',
  '[1:0:0:2:0:0:0:3]', '[0:0:1:0:0:1:0:0]', '[:1]', '[1:]', '[::1', '::1]', '[x]', '[0:0:0:0:0:0:0:0]',
  '１２７．０．０．１', '１２７。０。０。１', 'ｅｘａｍｐｌｅ．ｃｏｍ', 'a\u3002b', 'a\uFF0Eb', 'a\uFF61b',
  'bücher.example', 'xn--bcher-kva.example', 'XN--BCHER-KVA.example', 'xn--a.com', 'xn--', 'xn--abc-',
  'xn---', 'xn--55qx5d.cn', 'XN--55QX5D.cn', 'xn--zca', 'xn--zz', 'xn--a-ecp.ru', 'xn--ls8h',
  'xn--e28h.com', 'xn--1ch', 'xn--9a.com', 'xn--mgb0j', 'xn--4db', 'xn--4dbc', 'xn-a.com', 'xn-.com',
  'ß.de', 'ς.gr', 'faß.de', 'Ⅻ', 'ａ', 'İ.example', 'a\u0130b', '\u0130', 'K.example', 'ﬁ.example',
  'a\u00ADb', '\u00AD', 'a\u200Bb', 'a\u200Cb', 'a\u200Db', 'a\u094D\u200Cb', 'a\u094D\u200Db',
  '\u0628\u200C\u0628', '\u0300a', 'a\u0300', 'e\u0301', '\u05D0a', '\u05D0\u05D1', '1\u05D0',
  'a.\u05D0', '\u05D0.a', '\u0627\u0661', '\u0627\u06F1', '\u0627\u0661\u06F1', '\u05D0\u0300',
  '\u0661a', 'a\u0661', '\uFEFFa', 'a\uFE0Fb', '\u{E0100}a', '\u{1F600}', '\u{1F600}.com',
  'a\u2488b', '\u2488', 'a\u0080b', 'a\u00A0b', 'a\u2028b', 'a\uFFFDb', '\uFFFD', 'a*b', 'a_b', 'a$b',
  'a!b', "a'b", 'a(b', 'a,b', 'a;b', 'a=b', 'a{b', 'a}b', 'a~b', 'a"b', 'a`b', 'a<b', 'a>b', 'a^b',
  'a|b', 'a b', 'a%b', 'a\\b', 'a\u007Fb', '\u0007', 'ab\u0000', 'www.example.com', 'WWW.Example.COM',
  'localhost', 'LOCALHOST', 'localhost.', 'a'.repeat(63) + '.com', 'a'.repeat(64) + '.com',
  'a'.repeat(300), '\u4E2D\u6587.\u4E2D\u56FD', '\uD55C\uAD6D\uC5B4.kr', '\u1100\u1161\u11A8',
  'ā.example', 'a\u0304.example', '\u0915\u094D\u0937', 'xn--wgv71a119e.jp', 'xn--fiqs8s',
];

const inputs = [];
for (const host of hosts) {
  inputs.push(`http://${host}/`, `https://${host}:8080/p?q#f`);
}
inputs.push(
  'https://a.b/c.png', 'http:x', 'https:example.com', 'https:///x', 'https://', 'https://a b',
  'https://user@host/', 'https://user:pass@host/', 'https://us er:pa ss@host/', 'https://@host/',
  'https://:@host/', 'https://user:@host/', 'https://:pass@host/', 'https://a@b@c/', 'https://a:b:c@d/',
  'https://a%40b@c/', 'https://user@/', 'https://@/', 'https://user:pass@:80/',
  'https://host:99999/', 'https://host:/', 'https://host:0/', 'https://host:80/', 'https://host:443/',
  'http://host:80/', 'http://host:443/', 'http://host:065535/', 'http://host:65535/', 'http://host:65536/',
  'http://host:-1/', 'http://host:1a/', 'http://host:+1/', 'http://host: 1/', 'ws://host:80/',
  'wss://host:443/', 'ftp://host:21/', 'file://host/x', 'file:///etc/passwd', 'file:c:/x', 'file:///C|/x',
  'file://localhost/x', 'file:', 'file:x', 'file:/x/../y', 'file://C:/x', 'missing_small.png', 'HTTPS://X',
  '  https://x  ', '\u0000https://x\u001F', 'https://x/\u0000', 'ftp://x', 'javascript:alert(1)',
  'data:text/html,<b>', 'mailto:a@b', 'blob:https://a/b', 'foo://host/path', 'foo:/path', 'foo:path',
  'foo://', 'foo:', 'foo://user@host:9/x', 'foo://[::1]/', 'foo://a b/', 'foo://a<b/', 'foo://%41/',
  'foo://h\u00e9/', 'foo:////x', 'foo:/.//x', 'sc://ñ/', 'https://a\tb.com', 'https://a\nb.com',
  'https://a\rb.com', 'ht\ttps://x', 'https:\\\\host\\path', 'https:/\\host/', 'https:\\host',
  'https://host\\a\\b', 'https://a|b', 'https://a:b@c:8080/x?y#z', 'https://host/a/../b',
  'https://host/a/./b', 'https://host/a/%2e/b', 'https://host/a/%2E%2e/b', 'https://host/..',
  'https://host/../..', 'https://host/a/..', 'https://host/a/.', 'https://host/./', 'https://host//a',
  'https://host/a//b', 'https://host/a b', 'https://host/é', 'https://host/%zz', 'https://host/a%2fb',
  'https://host/{}', 'https://host/`', "https://host/'\"<>^|", 'https://host/?q=a b', "https://host/?q='\"",
  'https://host/?q=é', 'https://host/?a=1&b=2', 'https://host/?', 'https://host/#', 'https://host/?#',
  'https://host/# a`b<>"', 'https://host/#é', 'https://host/#x#y', 'https://host?x', 'https://host#x',
  'https://host:8080?x', 'https://host:8080#x', 'https://example.com.', 'https://\u00e9.com/',
  'https://\uFF21.com/', 'https://a.b/\uD800', 'https://\u0130/', 'https://xn--zz/', 'https://1.2.3.4/',
  'https://0x100000000/', 'https://4294967295/', 'https://1.2.3/', 'https://256.0.0.1/',
  'https://foo.09/', 'https://foo.0x/', 'https://a.b.c.1/', 'https://0x7f.1/', '', ' ', 'http://',
  'http:', 'http:/', 'http:///', 'http:////x', 'http:\\\\x', 'http://x:', 'http://[', 'http://]',
  'http://[]', 'http://[::1]x/', 'http://[::1]:80/', 'http://[::1]:8080/', 'http://x?y#z',
  'https://example.com/' + 'a'.repeat(9000),
);

const bases = ['https://news.example/a/b/c?q#f', 'https://user:pw@base.example:8443/dir/file',
  'https://[::1]/x', 'http://1.2.3.4/a/', 'file:///C:/dir/file', 'foo://host/a/b', 'foo:opaque',
  'https://xn--bcher-kva.example/'];
const relatives = ['//other.example/x', '//other.example', '///x', '\\\\other.example\\x', '/path',
  '/', '?q', '?', '#f', '#', '', '../x', '../../../../x', './x', 'x', '..', '.', '/..', '/./x',
  '%2e%2e/x', 'https://abs.example/y', 'https:rel', 'http:rel', 'http://abs/y', 'ftp://ftp.example/',
  'file:///x', 'javascript:alert(1)', 'data:,x', '//[::1]/', '//127.1/', '//169.254.169.254/latest',
  '//0x7f.1/', '//１２７．０．０．１/', '//bücher.example/', '//xn--a.com/', '//a b/', '//[', 'http://[',
  ':foo', 'C:/x', 'C|/x', '\\x', '//user@host/', '//:80/', '//host:99999/', '//host:8080/p', '?q=a b',
  '#a b', 'x\ty', ' /x ', '\u0000/x'];
const relativeCases = [];
for (const base of bases) for (const input of relatives) relativeCases.push({ input, base });

const canonicalInputs = [
  'https://www.coindesk.com/markets/2026/08/25/bitcoin/?utm_source=rss&utm_medium=x',
  'https://WWW.Example.com/a/b///?ref=x&a=1&REF=y&Source=s&fbclid=1&gclid=2&mc_cid=3&mc_eid=4&b=2#frag',
  'http://www.example.com:443/x/', 'http://www.example.com:80/x/', 'https://example.com',
  'https://example.com/', 'https://example.com/?', 'https://example.com/?a', 'https://example.com/?a=',
  'https://example.com/?=b', 'https://example.com/?a=b=c', 'https://example.com/?a+b=c+d',
  'https://example.com/?a%20b=%2B&%zz=1', 'https://example.com/?q=%C3%A9&r=%E9', 'https://example.com/?a&&b&',
  'https://example.com/?UTM_x=1&utm=2&reference=3&refx=4&sources=5&source=6',
  'https://example.com/?a=1&a=2&utm_a=3', "https://example.com/?a=*-._~!'()", 'https://www./x',
  'https://www.www.example/x', 'https://wwwexample.com/', 'https://www/x', 'http://user:pw@www.a.b/c',
  'https://[::1]/x/', 'https://1.2.3.4/x', 'http://0x7f.1/x', 'https://bücher.example/x',
  'https://example.com/a%2Fb/', 'https://example.com/%7Euser/', 'https://example.com/é/',
  'https://example.com/?é=ü', 'https://example.com/?a=%26b%3Dc', 'https://example.com/?a=1#x?y',
  'http://[', 'https://a b/', 'https://example.com/?q=a\u2028b', 'HTTPS://EXAMPLE.COM/PATH/',
  'https://example.com/path;params?x=1', 'https://example.com///', 'https://www.example.com.',
  'https://example.com/?%41=%42', 'https://example.com/?a%3D=b', 'https://example.com/?%3D',
];

// RFC 3492 Punycode encoding of a label (code points), no prefix.
function punycode(label) {
  const input = [...label].map((c) => c.codePointAt(0));
  const base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
  const adapt = (delta, points, first) => {
    delta = first ? Math.floor(delta / damp) : delta >> 1;
    delta += Math.floor(delta / points);
    let k = 0;
    while (delta > ((base - tmin) * tmax) >> 1) { delta = Math.floor(delta / (base - tmin)); k += base; }
    return k + Math.floor(((base - tmin + 1) * delta) / (delta + skew));
  };
  const digit = (d) => String.fromCharCode(d < 26 ? d + 97 : d + 22);
  let out = input.filter((c) => c < 0x80).map((c) => String.fromCharCode(c)).join('');
  const b = out.length;
  let h = b;
  if (b > 0) out += '-';
  let n = 128, delta = 0, bias = 72;
  while (h < input.length) {
    const m = Math.min(...input.filter((c) => c >= n));
    delta += (m - n) * (h + 1);
    n = m;
    for (const c of input) {
      if (c < n) delta++;
      if (c === n) {
        let q = delta;
        for (let k = base; ; k += base) {
          const t = k <= bias ? tmin : k >= bias + tmax ? tmax : k - bias;
          if (q < t) break;
          out += digit(t + ((q - t) % (base - t)));
          q = Math.floor((q - t) / (base - t));
        }
        out += digit(q);
        bias = adapt(delta, h + 1, h === b);
        delta = 0;
        h++;
      }
    }
    delta++;
    n++;
  }
  return out;
}

function sha256(text) {
  return createHash('sha256').update(text).digest('hex');
}

const contexts = [
  { prefix: 'http://a', suffix: 'b.example/' },
  { prefix: 'http://', suffix: '/' },
  { prefix: 'http://', suffix: '\u0308x.example/' },
  { prefix: 'http://x.', suffix: '' },
  { prefix: 'http://xn--', suffix: '.example/', before: 'a', after: '' },
  { prefix: 'http://xn--', suffix: '/', before: '', after: '' },
  { prefix: 'http://xn--', suffix: '/', before: '\u05D0', after: '1' },
];
const exhaustive = [];
for (const context of contexts) {
  const blocks = [];
  const total = createHash('sha256');
  let count = 0;
  for (let block = 0; block <= 0x10ffff >> 12; block++) {
    let text = '';
    for (let cp = Math.max(block << 12, 0x80); cp < (block + 1) << 12; cp++) {
      if (cp >= 0xd800 && cp <= 0xdfff) continue;
      let href;
      const middle = context.before === undefined ? String.fromCodePoint(cp)
        : punycode(`${context.before}${String.fromCodePoint(cp)}${context.after}`);
      try { href = new URL(`${context.prefix}${middle}${context.suffix}`).href; }
      catch { href = 'null'; }
      text += `${cp.toString(16)} ${href}\n`;
      count++;
    }
    blocks.push(sha256(text));
    total.update(text);
  }
  exhaustive.push({ ...context, count, sha256: total.digest('hex'), blocks });
}

const fixture = {
  source: {
    oracle: '`new URL(input, base)` in the production TypeScript runtime',
    image: IMAGE,
    node: process.versions.node,
    ada: process.versions.ada,
    icu: process.versions.icu,
    generator: 'server/intelligence_cpp/scripts/generate_whatwg_url_corpus.mjs',
  },
  cases: [
    ...inputs.map((input) => ({ input, result: describe(input) })),
    ...relativeCases.map(({ input, base }) => ({ input, base, result: describe(input, base) })),
  ],
  canonical: canonicalInputs.map((input) => ({ input, result: canonicalUrl(input) })),
  exhaustive,
};
process.stdout.write(`${JSON.stringify(fixture, null, 1)}\n`);
