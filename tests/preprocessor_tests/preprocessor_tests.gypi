# Copyright (c) 2014 The ANGLE Project Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

{
    'sources':
    [
        '<!@(python <(angle_path)/enumerate_files.py <(angle_path)/tests/preprocessor_tests/ -types *.cpp *.h)'
    ],
}

