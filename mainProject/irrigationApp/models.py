from django.db import models
from django.utils import timezone

class SolenoidControl(models.Model):
    MODE_CHOICES = [
        ("AUTO", "Auto"),
        ("MANUAL", "Manual Timer"),
        ("ON", "Force Open - Debug"),
        ("OFF", "Force Close - Debug"),
    ]

    mode = models.CharField(
        max_length=10,
        choices=MODE_CHOICES,
        default="AUTO"
    )

    # AUTO: closed for X time, open for Y time, repeat
    auto_closed_time = models.PositiveIntegerField(default=18000)  # 5 hours
    auto_open_time = models.PositiveIntegerField(default=20)       # 20 seconds

    # MANUAL TIMER: perform Action X at selected times
    manual_morning_time = models.TimeField(default="06:00")
    manual_afternoon_time = models.TimeField(default="18:00")
    manual_closed_time = models.PositiveIntegerField(default=10)
    manual_open_time = models.PositiveIntegerField(default=20)

    # Device status
    last_seen = models.DateTimeField(null=True, blank=True)

    def __str__(self):
        return f"Solenoid Control - {self.mode}"