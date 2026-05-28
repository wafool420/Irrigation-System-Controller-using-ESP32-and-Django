from datetime import timedelta

from django.shortcuts import render, redirect
from django.http import JsonResponse
from django.utils import timezone

from .models import SolenoidControl


def get_control():
    control, created = SolenoidControl.objects.get_or_create(id=1)
    return control


def dashboard(request):
    control = get_control()

    if request.method == "POST":
        mode = request.POST.get("mode")

        if mode in ["AUTO", "MANUAL", "ON", "OFF"]:
            control.mode = mode

        # AUTO SETTINGS
        auto_closed_hours = request.POST.get("auto_closed_hours")
        auto_closed_seconds = request.POST.get("auto_closed_seconds")
        auto_open_time = request.POST.get("auto_open_time")

        if auto_closed_seconds:
            control.auto_closed_time = int(auto_closed_seconds)
        elif auto_closed_hours:
            control.auto_closed_time = int(auto_closed_hours) * 3600

        if auto_open_time:
            control.auto_open_time = int(auto_open_time)

        # MANUAL TIMER SETTINGS
        morning_hour = request.POST.get("morning_hour")
        morning_minute = request.POST.get("morning_minute")

        afternoon_hour = request.POST.get("afternoon_hour")
        afternoon_minute = request.POST.get("afternoon_minute")

        manual_open_time = request.POST.get("manual_open_time")

        if morning_hour and morning_minute:
            control.manual_morning_time = (
                f"{int(morning_hour):02d}:{int(morning_minute):02d}"
            )

        if afternoon_hour and afternoon_minute:
            hour = int(afternoon_hour)

            # Convert PM to 24-hour format
            if hour != 12:
                hour += 12

            control.manual_afternoon_time = (
                f"{hour:02d}:{int(afternoon_minute):02d}"
            )

        if manual_open_time:
            control.manual_open_time = int(manual_open_time)

        control.save()
        return redirect("dashboard")

    auto_closed_hours = control.auto_closed_time // 3600
    auto_closed_seconds = control.auto_closed_time

    morning_hour = control.manual_morning_time.hour
    morning_minute = control.manual_morning_time.minute

    afternoon_hour_24 = control.manual_afternoon_time.hour
    afternoon_minute = control.manual_afternoon_time.minute

    if afternoon_hour_24 == 12:
        afternoon_hour = 12
    elif afternoon_hour_24 > 12:
        afternoon_hour = afternoon_hour_24 - 12
    else:
        afternoon_hour = afternoon_hour_24

    is_device_online = (
        control.last_seen is not None and
        timezone.now() - control.last_seen <= timedelta(seconds=10)
    )

    auto_mode_text = (
        f"Auto mode: water every {auto_closed_hours} hour(s) "
        f"for {control.auto_open_time} second(s)."
    )

    manual_mode_text = (
        f"Manual timer: waters at "
        f"{control.manual_morning_time.strftime('%I:%M %p')} and "
        f"{control.manual_afternoon_time.strftime('%I:%M %p')} "
        f"for {control.manual_open_time} second(s)."
    )

    context = {
        "control": control,

        "auto_closed_hours": auto_closed_hours,
        "auto_closed_seconds": auto_closed_seconds,

        "morning_hour": f"{morning_hour:02d}",
        "morning_minute": morning_minute,

        "afternoon_hour": f"{afternoon_hour:02d}",
        "afternoon_minute": afternoon_minute,

        "auto_mode_text": auto_mode_text,
        "manual_mode_text": manual_mode_text,
        "is_device_online": is_device_online,
    }

    return render(request, "irrigationApp/dashboard.html", context)


def esp32_command_api(request):
    control = get_control()

    control.last_seen = timezone.now()
    control.save()

    output_mode = control.mode

    # Django handles MANUAL TIMER clock logic.
    # ESP32 only receives ON/OFF/AUTO.
    if control.mode == "MANUAL":
        now = timezone.localtime().time()

        morning_time = control.manual_morning_time
        afternoon_time = control.manual_afternoon_time
        manual_duration = control.manual_open_time

        def is_within_watering_window(schedule_time):
            now_seconds = now.hour * 3600 + now.minute * 60 + now.second
            schedule_seconds = (
                schedule_time.hour * 3600
                + schedule_time.minute * 60
                + schedule_time.second
            )

            return schedule_seconds <= now_seconds < schedule_seconds + manual_duration

        if (
            is_within_watering_window(morning_time)
            or is_within_watering_window(afternoon_time)
        ):
            output_mode = "ON"
        else:
            output_mode = "OFF"

    data = {
        "mode": output_mode,
        "saved_mode": control.mode,
        "django_time": timezone.localtime().strftime("%H:%M:%S"),

        "auto_closed_time": control.auto_closed_time,
        "auto_open_time": control.auto_open_time,

        "manual_morning_time": control.manual_morning_time.strftime("%H:%M"),
        "manual_afternoon_time": control.manual_afternoon_time.strftime("%H:%M"),
        "manual_open_time": control.manual_open_time,
    }

    return JsonResponse(
        data,
        json_dumps_params={"separators": (",", ":")}
    )


def device_status_api(request):
    control = get_control()

    is_device_online = (
        control.last_seen is not None and
        timezone.now() - control.last_seen <= timedelta(seconds=10)
    )

    data = {
        "online": is_device_online,
        "last_seen": control.last_seen.strftime("%H:%M:%S") if control.last_seen else None,
    }

    return JsonResponse(data)