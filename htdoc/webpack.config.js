const path = require('path');
const webpack = require('webpack');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const MiniCssExtractPlugin = require('mini-css-extract-plugin');
const CopyWebpackPlugin = require('copy-webpack-plugin');
const pkg = require('./package.json');

// Source lives in htdoc/src; the production build is emitted into res/htdoc,
// which the C++ server serves as its static document root.
const outputDir = path.resolve(__dirname, '../res/htdoc');

// The C++ server listens on 8080 (HTTP_PORT in config.yml); proxy the API routes
// there in development so the dev-served client talks to a real backend.
const apiProxyTarget = 'http://localhost:8080';

module.exports = (env, argv) => {
  const isProduction = argv.mode === 'production';

  return {
    entry: {
      index: './src/index.js',
    },
    output: {
      path: outputDir,
      filename: 'assets/[name].js',
      assetModuleFilename: 'assets/[name][ext]',
      clean: true,
      // 'auto' makes webpack derive the public path at runtime from the bundle's
      // own script URL, so chunk/asset requests resolve under whatever sub-path
      // the app is mounted at (HTTP_URI_PREFIX). Combined with the runtime <base>
      // in index.html, the whole app is location-independent: one build serves
      // correctly from "/" or from "/mirobody" with no rebuild.
      publicPath: 'auto',
    },
    module: {
      rules: [
        {
          test: /\.css$/i,
          use: [MiniCssExtractPlugin.loader, 'css-loader'],
        },
        {
          test: /\.(svg|png|jpe?g|gif|webp|woff2?|ttf)$/i,
          type: 'asset/resource',
        },
      ],
    },
    plugins: [
      // Inject the package version so the About dialog can show it.
      new webpack.DefinePlugin({
        __APP_VERSION__: JSON.stringify(pkg.version),
      }),
      new HtmlWebpackPlugin({
        template: './src/index.html',
        filename: 'index.html',
        chunks: ['index'],
        favicon: './src/assets/mirobody.svg',
        minify: isProduction,
      }),
      new MiniCssExtractPlugin({
        filename: 'assets/[name].css',
      }),
      // Vendored static assets served at the doc-root (not webpack-bundled): the
      // QR generator and Tanka's WASM request-signer glue, which the Tanka login
      // panel loads at runtime via <base>/qrcode.min.js and <base>/tanka-signer.js
      // (a native dynamic import kept out of the bundle). The matching WASM is
      // bridged by the C++ server at /tanka-sign.wasm. tanka-signer.js is a
      // verified, matched set with that WASM -- regenerate both via
      // `node res/tanka/discover.cjs --write`, never hand-edit.
      new CopyWebpackPlugin({
        patterns: [{ from: 'static', to: '.' }],
      }),
    ],
    devServer: {
      static: { directory: outputDir },
      port: 8090,
      open: true,
      hot: true,
      proxy: [
        {
          context: ['/api', '/email', '/v1', '/user', '/google', '/wechat', '/apple',
                    '/github', '/firebase', '/tanka', '/tanka-sign.wasm'],
          target: apiProxyTarget,
          changeOrigin: true,
        },
      ],
    },
  };
};
