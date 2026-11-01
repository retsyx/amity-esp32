#!/usr/bin/env python3
"""Compile the actual startup profile with legacy, empty and sparse settings."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include "board_pins.h"
#include <cassert>
int main() {
 const auto& profile=amity::board_profile;
 assert(profile.tentacle_count==EXPECTED_COUNT);
 unsigned mask=0;for(unsigned id=0;id<profile.tentacle_count;++id)if(cec::connector_enabled(profile,id))mask|=1u<<id;
 assert(mask==EXPECTED_MASK);
 const auto output=[](int pin){return pin>=0&&pin<49&&pin!=46;};
 const auto input=[](int pin){return pin>=0&&pin<49;};
 assert((cec::validate_board_profile(profile,output,nullptr,0,input)==cec::ProfileError::None)==EXPECTED_VALID);
}
'''


def settings(pairs):
    lines = ['#define CONFIG_AMITY_BOARD_DEVELOPMENT_4T 1', '#define CONFIG_AMITY_BOARD_RESET_GPIO -1']
    for index in range(4):
        front, back = pairs.get(index, (-1, -1))
        lines += [f'#define CONFIG_AMITY_TENTACLE{index}_FRONT_GPIO {front}',
                  f'#define CONFIG_AMITY_TENTACLE{index}_BACK_GPIO {back}']
    return '\n'.join(lines) + '\n'


cases = [('legacy', '', 1, 1, True),
         ('empty', settings({}), 4, 0, True),
         ('sparse-fourth', settings({3: (9, 44)}), 4, 8, True),
         ('full', settings({0: (1, 2), 1: (5, 6), 2: (7, 8), 3: (9, 44)}), 4, 15, True),
         ('partial-pair', settings({3: (9, -1)}), 4, 0, False),
         ('cec-collision', settings({2: (9, 44), 3: (9, 44)}), 4, 12, False)]
with tempfile.TemporaryDirectory(prefix='amity-profile-') as directory:
    temporary = Path(directory)
    (temporary/'test.cpp').write_text(source)
    for name, config, count, mask, valid in cases:
        (temporary/'sdkconfig.h').write_text(config)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(temporary), '-I'+str(root/'main'), '-I'+str(root/'components/cec/include'), '-I'+str(root/'components/amity_system/include'),
                        f'-DEXPECTED_COUNT={count}', f'-DEXPECTED_MASK={mask}', f'-DEXPECTED_VALID={int(valid)}',
                        str(temporary/'test.cpp'), '-o', str(temporary/'test')], check=True)
        subprocess.run([str(temporary/'test')], check=True)
        print('PASS startup profile:', name)
