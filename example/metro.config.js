const path = require('path');
const { getDefaultConfig } = require('@expo/metro-config');
const { withMetroConfig } = require('react-native-monorepo-config');

const root = path.resolve(__dirname, '..');

/**
 * Metro configuration
 * https://facebook.github.io/metro/docs/configuration
 *
 * @type {import('metro-config').MetroConfig}
 */
const config = withMetroConfig(getDefaultConfig(__dirname), {
  root,
  dirname: __dirname,
  conditions: ['react-native-nitro-rtmp-source'],
});

// The C++ test fixtures (cpp/__tests__/fixtures) are imported as assets by
// src/fixtures.ts; the monorepo config already watches the repository root.
config.resolver.assetExts = [...config.resolver.assetExts, 'h264', 'aac'];

// Metro blocks every `__tests__` directory by default (metro-config's
// exclusionList), and Expo folds that default into one combined RegExp. Rebuild
// the list so that only cpp/__tests__/fixtures is let through.
const escapeForBlockList = (pattern) =>
  pattern
    .replace(/[-[\]{}()*+?.\\^$|]/g, '\\$&')
    .replaceAll('/', `\\${path.sep}`);
config.resolver.blockList = [
  ...[config.resolver.blockList]
    .flat()
    .filter(
      (entry) =>
        entry != null &&
        !(entry instanceof RegExp && entry.source.includes('__tests__'))
    ),
  new RegExp(`${escapeForBlockList('.expo/types')}$`),
  // The crawler also tests directories (no trailing separator), hence the `|$`.
  new RegExp(
    `\\${path.sep}__tests__\\${path.sep}(?!fixtures(?:\\${path.sep}|$))`
  ),
];

module.exports = config;
