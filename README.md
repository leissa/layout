# MimIR Layout Plugin

A prototype *tensor layout algebra* for MimIR, following the Axe layout model
(Hou et al., "Axe: A Simple Unified Layout Abstraction for Machine Learning Compilers").
A layout describes how the elements of a logical tensor are placed onto named
hardware resources (registers, lanes, warps, memory, devices, ...).

The full specification — types, axioms, normalizer semantics, and the
"Future Work" section — lives in [`layout.mim`](layout.mim); this README only
covers building and testing the plugin.

## Building

```sh
git clone --recursive https://github.com/mimir/mimir.git
cd mimir/extra
git clone git@github.com:leissa/layout.git
```

Then, [build MimIR as usual](https://mimir.github.io/coding.html#building).

## Testing

The `lit/` directory holds regression tests (`canon.mim`, `tile.mim`,
`untile.mim`, `slice.mim`, `eval.mim`); once MimIR is configured with
`-DBUILD_TESTING=ON`, they are picked up automatically by the main `lit`
target.
