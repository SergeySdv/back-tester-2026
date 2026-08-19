"""Shared PyTorch architectures used by strategies and offline validation."""

import torch.nn as nn


class MLP(nn.Module):
    def __init__(self, n_features):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(n_features, 64),
            nn.ReLU(),
            nn.Linear(64, 32),
            nn.ReLU(),
            nn.Linear(32, 1),
        )

    def forward(self, x):
        return self.net(x).squeeze(-1)


class CausalConv1d(nn.Module):
    def __init__(self, in_ch, out_ch, kernel_size, dilation):
        super().__init__()
        self.pad = (kernel_size - 1) * dilation
        self.conv = nn.Conv1d(
            in_ch,
            out_ch,
            kernel_size,
            padding=self.pad,
            dilation=dilation,
        )

    def forward(self, x):
        out = self.conv(x)
        return out[:, :, : -self.pad] if self.pad > 0 else out


class TCNBlock(nn.Module):
    def __init__(self, in_ch, out_ch, kernel_size, dilation):
        super().__init__()
        self.conv1 = CausalConv1d(in_ch, out_ch, kernel_size, dilation)
        self.relu1 = nn.ReLU()
        self.conv2 = CausalConv1d(out_ch, out_ch, kernel_size, dilation)
        self.relu2 = nn.ReLU()
        self.downsample = nn.Conv1d(in_ch, out_ch, 1) if in_ch != out_ch else None

    def forward(self, x):
        out = self.relu1(self.conv1(x))
        out = self.relu2(self.conv2(out))
        residual = x if self.downsample is None else self.downsample(x)
        return out + residual


class TCN(nn.Module):
    def __init__(self, n_features, channels=(32, 32, 32), kernel_size=3):
        super().__init__()
        layers = []
        in_ch = n_features
        for index, out_ch in enumerate(channels):
            layers.append(TCNBlock(in_ch, out_ch, kernel_size, 2**index))
            in_ch = out_ch
        self.tcn = nn.Sequential(*layers)
        self.head = nn.Linear(in_ch, 1)

    def forward(self, x):
        out = self.tcn(x.transpose(1, 2))
        return self.head(out[:, :, -1]).squeeze(-1)


class LSTM(nn.Module):
    def __init__(self, n_features, hidden_size=32, num_layers=1):
        super().__init__()
        self.lstm = nn.LSTM(n_features, hidden_size, num_layers, batch_first=True)
        self.head = nn.Linear(hidden_size, 1)

    def forward(self, x):
        out, _ = self.lstm(x)
        return self.head(out[:, -1, :]).squeeze(-1)
