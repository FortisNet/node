import assert from 'node:assert/strict';
import { routeBindings } from '../scripts/route_bindings.mjs';
assert.deepEqual(routeBindings('router.Add("GET", "/v1/example", handler);'), [['GET', '/v1/example']]);
assert.deepEqual(routeBindings('router.Add<&Service::Post>(\n "POST", "/v1/example", service);'), [['POST', '/v1/example']]);
assert.deepEqual(routeBindings('constexpr char kRoute[] = "/v1/prices/missing-identifiers";\nrouter.Add<&Service::Post>("POST", kRoute, service);'),
  [['POST', '/v1/prices/missing-identifiers']]);
assert.deepEqual(routeBindings('router.Add("GET", dynamic_path, handler);'), []);
console.log('route binding extraction: literal, typed, constant and dynamic cases passed');
