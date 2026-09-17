"""
Spatial Downmix Engine - Python Reference Implementation
"""

import math
from enum import IntEnum
from dataclasses import dataclass
from typing import List, Optional


class SpatialLayout(IntEnum):
    STEREO = 0
    LAYOUT_5_1 = 1
    LAYOUT_7_1 = 2


class SpatialMatrixMode(IntEnum):
    NONE = 0
    DPLII = 1


class SpatialContentType(IntEnum):
    CBA = 0
    OBA = 1


@dataclass
class Gains5_1:
    left: float = 1.0
    right: float = 1.0
    center: float = 0.90
    surround: float = 0.55
    lfe: float = 0.25


@dataclass
class Gains7_1:
    left: float = 1.0
    right: float = 1.0
    center: float = 0.760
    side: float = 0.780
    rear: float = 0.620
    lfe: float = 0.030


@dataclass
class SpatialDownmixConfig:
    layout: SpatialLayout = SpatialLayout.LAYOUT_5_1
    gains_5_1: Gains5_1 = None
    gains_7_1: Gains7_1 = None
    matrix_oba: SpatialMatrixMode = SpatialMatrixMode.NONE
    matrix_cba: SpatialMatrixMode = SpatialMatrixMode.NONE
    content_type: SpatialContentType = SpatialContentType.CBA
    debug_enabled: bool = False

    def __post_init__(self):
        if self.gains_5_1 is None:
            self.gains_5_1 = Gains5_1()
        if self.gains_7_1 is None:
            self.gains_7_1 = Gains7_1()


class SpatialDownmix:
    def __init__(self, config: Optional[SpatialDownmixConfig] = None):
        self.config = config or SpatialDownmixConfig()
        # Make a deep copy to avoid reference issues
        if config:
            self.config = SpatialDownmixConfig(
                layout=config.layout,
                gains_5_1=Gains5_1(
                    left=config.gains_5_1.left,
                    right=config.gains_5_1.right,
                    center=config.gains_5_1.center,
                    surround=config.gains_5_1.surround,
                    lfe=config.gains_5_1.lfe,
                ),
                gains_7_1=Gains7_1(
                    left=config.gains_7_1.left,
                    right=config.gains_7_1.right,
                    center=config.gains_7_1.center,
                    side=config.gains_7_1.side,
                    rear=config.gains_7_1.rear,
                    lfe=config.gains_7_1.lfe,
                ),
                matrix_oba=config.matrix_oba,
                matrix_cba=config.matrix_cba,
                content_type=config.content_type,
                debug_enabled=config.debug_enabled,
            )
        self._matrix: List[List[float]] = []
        self._matrix_valid = False
        self._current_layout = SpatialLayout.STEREO
        self._ensure_matrix()

    def _build_matrix_5_1(self):
        g = self.config.gains_5_1
        self._matrix = [[0.0, 0.0] for _ in range(6)]

        self._matrix[0][0] = g.left
        self._matrix[1][1] = g.right

        c_to_lr = 0.7071067811865475
        self._matrix[2][0] = g.center * c_to_lr
        self._matrix[2][1] = g.center * c_to_lr

        s_to_lr = 0.7071067811865475
        self._matrix[4][0] = g.surround * s_to_lr
        self._matrix[4][1] = g.surround * s_to_lr
        self._matrix[5][0] = g.surround * s_to_lr
        self._matrix[5][1] = g.surround * s_to_lr

        lfe_to_lr = 1.0
        self._matrix[3][0] = g.lfe * lfe_to_lr
        self._matrix[3][1] = g.lfe * lfe_to_lr

        self._matrix_valid = True
        self._current_layout = SpatialLayout.LAYOUT_5_1

    def _build_matrix_7_1(self):
        g = self.config.gains_7_1
        self._matrix = [[0.0, 0.0] for _ in range(8)]

        self._matrix[0][0] = g.left
        self._matrix[1][1] = g.right

        c_to_lr = 0.7071067811865475
        self._matrix[2][0] = g.center * c_to_lr
        self._matrix[2][1] = g.center * c_to_lr

        side_to_lr = 0.7071067811865475
        self._matrix[4][0] = g.side * side_to_lr
        self._matrix[4][1] = g.side * side_to_lr
        self._matrix[5][0] = g.side * side_to_lr
        self._matrix[5][1] = g.side * side_to_lr

        rear_to_lr = 0.7071067811865475
        self._matrix[6][0] = g.rear * rear_to_lr
        self._matrix[6][1] = g.rear * rear_to_lr
        self._matrix[7][0] = g.rear * rear_to_lr
        self._matrix[7][1] = g.rear * rear_to_lr

        lfe_to_lr = 1.0
        self._matrix[3][0] = g.lfe * lfe_to_lr
        self._matrix[3][1] = g.lfe * lfe_to_lr

        self._matrix_valid = True
        self._current_layout = SpatialLayout.LAYOUT_7_1

    def _ensure_matrix(self):
        if not self._matrix_valid or self._current_layout != self.config.layout:
            if self.config.layout == SpatialLayout.LAYOUT_5_1:
                self._build_matrix_5_1()
            elif self.config.layout == SpatialLayout.LAYOUT_7_1:
                self._build_matrix_7_1()
            else:
                self._matrix = [[1.0, 0.0], [0.0, 1.0]]
                self._matrix_valid = True
                self._current_layout = SpatialLayout.STEREO

            if self.config.debug_enabled:
                print(f"[spatial_downmix] Matrix rebuilt for layout={self.config.layout.name}")
                for i, row in enumerate(self._matrix):
                    print(f"  Ch{i}: L={row[0]:.6f} R={row[1]:.6f}")

    def update_config(self, config: SpatialDownmixConfig):
        layout_changed = self.config.layout != config.layout
        gains_changed = (
            self.config.gains_5_1.left != config.gains_5_1.left or
            self.config.gains_5_1.right != config.gains_5_1.right or
            self.config.gains_5_1.center != config.gains_5_1.center or
            self.config.gains_5_1.surround != config.gains_5_1.surround or
            self.config.gains_5_1.lfe != config.gains_5_1.lfe or
            self.config.gains_7_1.left != config.gains_7_1.left or
            self.config.gains_7_1.right != config.gains_7_1.right or
            self.config.gains_7_1.center != config.gains_7_1.center or
            self.config.gains_7_1.side != config.gains_7_1.side or
            self.config.gains_7_1.rear != config.gains_7_1.rear or
            self.config.gains_7_1.lfe != config.gains_7_1.lfe
        )
        
        # Copy the config values instead of reassigning reference
        self.config.layout = config.layout
        self.config.gains_5_1.left = config.gains_5_1.left
        self.config.gains_5_1.right = config.gains_5_1.right
        self.config.gains_5_1.center = config.gains_5_1.center
        self.config.gains_5_1.surround = config.gains_5_1.surround
        self.config.gains_5_1.lfe = config.gains_5_1.lfe
        self.config.gains_7_1.left = config.gains_7_1.left
        self.config.gains_7_1.right = config.gains_7_1.right
        self.config.gains_7_1.center = config.gains_7_1.center
        self.config.gains_7_1.side = config.gains_7_1.side
        self.config.gains_7_1.rear = config.gains_7_1.rear
        self.config.gains_7_1.lfe = config.gains_7_1.lfe
        self.config.matrix_oba = config.matrix_oba
        self.config.matrix_cba = config.matrix_cba
        self.config.content_type = config.content_type
        self.config.debug_enabled = config.debug_enabled
        
        if layout_changed or gains_changed:
            self._matrix_valid = False
        self._ensure_matrix()

    def process(self, input_frames: List[List[float]]) -> List[List[float]]:
        self._ensure_matrix()

        num_input_channels = len(self._matrix)
        if num_input_channels == 0:
            num_input_channels = 2

        output = []
        for frame in input_frames:
            l = 0.0
            r = 0.0
            for c in range(min(num_input_channels, len(frame))):
                l += frame[c] * self._matrix[c][0]
                r += frame[c] * self._matrix[c][1]
            output.append([l, r])
        return output

    def process_interleaved(self, input_data: List[float], frames: int) -> List[float]:
        self._ensure_matrix()

        num_input_channels = len(self._matrix)
        if num_input_channels == 0:
            num_input_channels = 2

        output = [0.0] * (frames * 2)
        for f in range(frames):
            l = 0.0
            r = 0.0
            base = f * num_input_channels
            for c in range(min(num_input_channels, len(input_data) - base)):
                l += input_data[base + c] * self._matrix[c][0]
                r += input_data[base + c] * self._matrix[c][1]
            output[f * 2 + 0] = l
            output[f * 2 + 1] = r
        return output


def get_default_config_5_1() -> SpatialDownmixConfig:
    return SpatialDownmixConfig(
        layout=SpatialLayout.LAYOUT_5_1,
        gains_5_1=Gains5_1(),
        gains_7_1=Gains7_1(),
        matrix_oba=SpatialMatrixMode.NONE,
        matrix_cba=SpatialMatrixMode.NONE,
        content_type=SpatialContentType.CBA,
        debug_enabled=False
    )


def get_default_config_7_1() -> SpatialDownmixConfig:
    config = get_default_config_5_1()
    config.layout = SpatialLayout.LAYOUT_7_1
    return config