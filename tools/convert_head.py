"""Converts LAION's aesthetic linear head (.pth, torch zip format) to raw float32 without torch.

Usage: python convert_head.py sa_0_4_vit_b_32_linear.pth ai/aesthetic_b32.bin
Output: 512 float32 weights followed by 1 float32 bias.
"""
import pickle
import sys
import zipfile

import numpy as np


class _Storage:
    def __init__(self, key, dtype):
        self.key, self.dtype = key, dtype


def _rebuild_tensor(storage, offset, size, stride, *args):
    return ("tensor", storage, offset, tuple(size))


class _Unpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if name == "_rebuild_tensor_v2":
            return _rebuild_tensor
        if module == "collections" and name == "OrderedDict":
            import collections
            return collections.OrderedDict
        if name.endswith("Storage"):
            return name  # dtype marker
        raise pickle.UnpicklingError(f"unexpected class {module}.{name}")

    def persistent_load(self, pid):
        _, dtype, key, _loc, _n = pid
        return _Storage(key, dtype)


def main(src, dst):
    z = zipfile.ZipFile(src)
    root = [n for n in z.namelist() if n.endswith("data.pkl")][0].rsplit("/", 1)[0]
    state = _Unpickler(z.open(f"{root}/data.pkl")).load()
    tensors = {}
    for k, (_, st, off, size) in state.items():
        assert st.dtype == "FloatStorage", st.dtype
        raw = np.frombuffer(z.read(f"{root}/data/{st.key}"), dtype="<f4")
        tensors[k] = raw[off: off + int(np.prod(size))].reshape(size)
        print(k, size)
    w = tensors["weight"].reshape(-1)
    b = tensors["bias"].reshape(-1)
    assert w.size == 512 and b.size == 1
    np.concatenate([w, b]).astype("<f4").tofile(dst)
    print("wrote", dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
