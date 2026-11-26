// Extract statically named HttpRouter bindings for OpenAPI coverage checks.
// Dynamic route factories remain covered by their own route/contract tests.
export function routeBindings(source) {
  const constants = new Map([...source.matchAll(/constexpr\s+(?:const\s+)?char\s+(\w+)\s*(?:\[\s*\]|\*)\s*=\s*"([^"\n]+)"/g)]
    .map(([, name, value]) => [name, value]));
  return [...source.matchAll(/router\.Add(?:<[^;{}]*?>)?\(\s*"(GET|POST|PUT|PATCH|DELETE)"\s*,\s*(?:"([^"\n]+)"|(\w+))/g)]
    .map(([, method, literal, constant]) => [method, literal ?? constants.get(constant)])
    .filter(([, route]) => route !== undefined);
}
